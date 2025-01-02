#pragma once

#include <memory>
#include <array>
#include <vector>
#include <functional>
#include <limits>
#include <thread>
#include <unistd.h>
#include <cstring>

/*
    will create a thread for each added task, they will be executed in the same order.
    producer task, must be added, its responsibility is to fill the data parameter.
    internally it keeps a ring buffer of Len size for all the data structs.
*/
template<size_t Len, typename T>
class pipeLine final
{
    public:
    ~pipeLine() { stop(); }

    void addProducer(std::function<void(T&)>, int core = -1);
    void addProcessor(std::function<void(T&)>, int core = -1);
    void addFinalizer(std::function<void(T&)>, int core = -1);

    void start();
    void stop();

    private:
    void verifyNoUnfinishedTasks();
    
    static constexpr int flagIdleVal() { return 0; }
    struct DataNode
    {
        std::atomic<int> _flags{flagIdleVal()};
        T _data;
    };

    std::array<DataNode, Len> _ringBuffer;
    std::vector<std::thread> _threads;
    
    std::atomic<bool> _endProducing{false};
    std::atomic<bool> _endProcessing{false};

    struct TaskNode
    {
        TaskNode(std::function<void(T&)> task, int core): _task{std::move(task)}, _core{core} {}

        std::function<void(T&)> _task;
        int _core{-1};
    };

    std::vector<TaskNode> _tasks;
    size_t _limitNumOfTasks{std::numeric_limits<unsigned char>::max()};
};


inline void pinThreadOnCore(int core)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0)
    {
        const auto localErrno{errno};
        std::cerr << "Failed to pin core: " << core << ", rc: " << rc << ", errno: " << std::strerror(localErrno) << std::endl;
    }
}


template<size_t Len, typename T>
void pipeLine<Len, T>::addProducer(std::function<void(T&)> func_, int core)
{
    if (_tasks.size() != 0)
    {
        throw std::runtime_error{"producer must be first"};
    }
    _tasks.emplace_back(std::move(func_), core);
}
template<size_t Len, typename T>
void pipeLine<Len, T>::addProcessor(std::function<void(T&)> func_, int core)
{
    if (_limitNumOfTasks <= _tasks.size())
    {
        throw std::runtime_error{"finalizer was already added, can't add more processors"};
    }
    _tasks.emplace_back(std::move(func_), core);
}

template<size_t Len, typename T>
void pipeLine<Len, T>::addFinalizer(std::function<void(T&)> func_, int core)
{
    if (_limitNumOfTasks <= _tasks.size())
    {
        throw std::runtime_error{"finalizer was already added, can't another finalizer"};
    }
    _tasks.emplace_back(std::move(func_), core);
    _limitNumOfTasks = _tasks.size();
}

template<size_t Len, typename T>
void pipeLine<Len, T>::start()
{
    if(_tasks.size() < 2)
    {
        throw std::runtime_error{"must have at least 2 tasks - producer, (optional N processors) and finalizer"};
    }

    auto dataProducerTask{[this](int waitFlag_, int startFlag_, int endFlag_, TaskNode taskNode_){

        if (taskNode_._core > -1)
        {
            pinThreadOnCore(taskNode_._core);
        }

		size_t index{0};
		while(!this->_endProducing.load(std::memory_order_acquire))
		{
			auto& current{this->_ringBuffer[index++ % this->_ringBuffer.size()]};

			auto expected{waitFlag_};
			while(!current._flags.compare_exchange_strong(expected, startFlag_, std::memory_order_acq_rel))
			{
				expected = waitFlag_;
				//std::this_thread::yield();

				if(this->_endProducing.load(std::memory_order_acquire)) { return; }
			}

			taskNode_._task(current._data);
			current._flags.store(endFlag_, std::memory_order_release);
		}
	}};

    _endProducing.store(false, std::memory_order_release);

    auto iterProducer{_tasks.begin()};
    int waitFlag{flagIdleVal()};
    int startFlag{waitFlag + 1};
    int endFlag{waitFlag + 2};
    _threads.emplace_back(dataProducerTask, waitFlag, startFlag, endFlag, *iterProducer);
    waitFlag = endFlag;

    auto dataProcessorTask{[this](int waitFlag_, int startFlag_, int endFlag_, TaskNode taskNode_){

        if (taskNode_._core > -1)
        {
            pinThreadOnCore(taskNode_._core);
        }

		size_t index{0};
		while(!this->_endProcessing.load(std::memory_order_acquire))
		{
			auto& current{this->_ringBuffer[index % this->_ringBuffer.size()]};

			bool exitLoop{false};
			auto expected{waitFlag_};
			while(!current._flags.compare_exchange_strong(expected, startFlag_, std::memory_order_acq_rel))
			{
				expected = waitFlag_;
				//std::this_thread::yield();

				exitLoop = this->_endProcessing.load(std::memory_order_acquire);
				if(exitLoop) { break; }
			}

			if (exitLoop) { break; }

			taskNode_._task(current._data);
			current._flags.store(endFlag_, std::memory_order_release);
            ++index;
		}

		// finish queue
        const auto endTP{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
		while (std::chrono::steady_clock::now() < endTP)
		{
            bool exitLoop{false};
			auto& current{this->_ringBuffer[index++ % this->_ringBuffer.size()]};
			auto expected{waitFlag_};
			while(!current._flags.compare_exchange_strong(expected, startFlag_, std::memory_order_acq_rel))
			{
				if (expected == flagIdleVal())
				{
                    // all good, no more to process
					return;
				}

                exitLoop = std::chrono::steady_clock::now() > endTP;
                if (exitLoop) { break; }

				expected = waitFlag_;
			}

            if (exitLoop) { break; }

			taskNode_._task(current._data);
			current._flags.store(endFlag_, std::memory_order_release);
		}

        std::cerr << "Failed to process some items, timed out" << std::endl;
	}};

    _endProcessing.store(false, std::memory_order_release);
    auto iterProcessor{iterProducer + 1};
    for (; iterProcessor != _tasks.end() - 1 ; ++iterProcessor)
    {
        startFlag = waitFlag + 1;
        endFlag = waitFlag + 2;
        _threads.emplace_back(dataProcessorTask, waitFlag, startFlag, endFlag, *iterProcessor);
        waitFlag = endFlag;
    }

    auto iterFinalizer{iterProcessor};
    startFlag = waitFlag + 1;
    endFlag = flagIdleVal();
    _threads.emplace_back(dataProcessorTask, waitFlag, startFlag, endFlag, *iterFinalizer);
}

template<size_t Len, typename T>
void pipeLine<Len, T>::stop()
{
    auto iterProducer{_threads.begin()};
    if (iterProducer == _threads.end())
    {
        return;
    }

    _endProducing.store(true, std::memory_order_release);
    if (iterProducer->joinable())
    {
        iterProducer->join();
    }

    _endProcessing.store(true, std::memory_order_release);
    for (auto iter = iterProducer + 1 ; iter != _threads.end() ; ++iter)
    {
        if (iter->joinable())
        {
            iter->join();
        }
    }

    _threads.clear();
    verifyNoUnfinishedTasks();
}

template<size_t Len, typename T>
void pipeLine<Len, T>::verifyNoUnfinishedTasks()
{
    for (auto& data : _ringBuffer)
    {
        if (data._flags.load(std::memory_order_acquire) != flagIdleVal())
        {
            std::cerr << "Error: ringBuffer contains non idle element" << std::endl;
        }
    }
}