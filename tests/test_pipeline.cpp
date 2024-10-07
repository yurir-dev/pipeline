
#include <iostream>
#include <atomic>
#include <vector>
#include <array>
#include <thread>
#include <mutex>
#include <functional>
#include <math.h>

#include "../pipeline.h"

void testAtomicFlag()
{
	std::atomic_flag f{ATOMIC_FLAG_INIT};
	std::cout << "f 1: " << f.test_and_set() << std::endl;
	std::cout << "f 2: " << f.test_and_set() << std::endl;
	std::cout << "f 3: " << f.test_and_set() << std::endl;

	f.clear();
	std::cout << "f 10: " << f.test_and_set() << std::endl;
	std::cout << "f 20: " << f.test_and_set() << std::endl;
	std::cout << "f 30: " << f.test_and_set() << std::endl;
}

void testCompareExchange()
{
	std::atomic<bool> atomicVar{true}; 

	bool expected{false};
	{
		auto res{atomicVar.compare_exchange_weak(expected, true)};
		std::cout << "compare_exchange_weak: res: " << res << ", expected: " << expected << ", atomicVar: " << atomicVar.load() << std::endl;
	}
	{
		auto res{atomicVar.compare_exchange_weak(expected, true)};
		std::cout << "compare_exchange_weak: res: " << res << ", expected: " << expected << ", atomicVar: " << atomicVar.load() << std::endl;
	}
}


template <typename data_t, typename flags_t>
struct DataNode
{
	std::atomic<flags_t> _flags;
	data_t _data;
};
struct DataToProcess
{
	std::vector<std::string> _results; // every task appends some result string
};

void testQueueProcessing()
{
	enum class flags
	{
		idle,
		
		producerStart,
		producerEnd,
		
		task1Start,
		task1End,
		
		task2Start,
		task2End,

		task3Start,
		task3End,

		task4Start,
	};

	std::atomic<bool> endProducer{false};
	std::atomic<bool> endProcessors{false};
	std::array<DataNode<DataToProcess, flags>, 128> queue;
	for (auto& d : queue)
	{
		d._flags = flags::idle;
	}

	auto dataProducerTask{[&queue, &endProducer](flags waitFlag_, flags startFlag_, flags endFlag_, std::function<void(DataToProcess&)> proc_){
		size_t index{0};
		while(!endProducer.load(std::memory_order_acquire))
		{
			auto& current{queue[index++ % queue.size()]};

			auto expected{waitFlag_};
			while(!current._flags.compare_exchange_strong(expected, startFlag_, std::memory_order_acq_rel))
			{
				expected = waitFlag_;
				//std::this_thread::yield();

				if(endProducer.load(std::memory_order_acquire)) { return; }
			}

			proc_(current._data);
			current._flags.store(endFlag_, std::memory_order_release);
		}
	}};


	auto dataTask{[&queue, &endProcessors](flags waitFlag_, flags startFlag_, flags endFlag_, std::function<void(DataToProcess&)> proc_){
		size_t index{0};
		while(!endProcessors.load(std::memory_order_acquire))
		{
			auto& current{queue[index++ % queue.size()]};

			bool exitLoop{false};
			auto expected{waitFlag_};
			while(!current._flags.compare_exchange_strong(expected, startFlag_, std::memory_order_acq_rel))
			{
				expected = waitFlag_;
				//std::this_thread::yield();

				exitLoop = endProcessors.load(std::memory_order_acquire);
				if(exitLoop) { break; }
			}

			if (exitLoop) { break; }

			proc_(current._data);
			current._flags.store(endFlag_, std::memory_order_release);
		}

		// finish queue
		while (true)
		{
			auto& current{queue[index++ % queue.size()]};
			auto expected{waitFlag_};
			while(!current._flags.compare_exchange_strong(expected, startFlag_, std::memory_order_acq_rel))
			{
				if (expected == flags::idle)
				{
					return;
				}
				expected = waitFlag_;
			}
			proc_(current._data);
			current._flags.store(endFlag_, std::memory_order_release);
		}
	}};

	std::vector<std::thread> threads;
	// data producer thread
	auto dataProducerThread{std::thread{dataProducerTask, flags::idle, flags::producerStart, flags::producerEnd, [](DataToProcess& data_){ data_ = DataToProcess{};}}};
	
	// data processors threads
	//auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 0; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	auto waistTime{[](){ std::this_thread::sleep_for(std::chrono::microseconds{10});}};
	auto task1{[&waistTime](DataToProcess& data_){data_._results.emplace_back("task1 worked"); waistTime();}};
	auto task2{[&waistTime](DataToProcess& data_){data_._results.emplace_back("task2 worked"); waistTime();}};
	auto task3{[&waistTime](DataToProcess& data_){data_._results.emplace_back("task3 worked"); waistTime();}};
	
	threads.emplace_back(dataTask, flags::producerEnd, flags::task1Start, flags::task1End, task1);
	threads.emplace_back(dataTask, flags::task1End, flags::task2Start, flags::task2End, task2);
	threads.emplace_back(dataTask, flags::task2End, flags::task3Start, flags::task3End, task3);

	// final processor - finalizes data

	size_t dataProcessedNum{0};
	auto finalizerTask{[&dataProcessedNum](DataToProcess& data_){
		data_._results.emplace_back("finished processing");

		const std::vector<std::string> expectedResults{"task1 worked", "task2 worked", "task3 worked", "finished processing"};

		if (expectedResults.size() != data_._results.size())
		{
			std::cout << "Error: data size: " << data_._results.size() << " does not match, expected: " << expectedResults.size() << std::endl;
			for (const auto& res : data_._results)
			{
				std::cout << res << ','; 
			}
			std::cout << std::endl;
		}
		else
		{
			for (size_t i = 0 ; i < expectedResults.size() ; ++i)
			{
				if (expectedResults[i] != data_._results[i])
				{
					std::cout << "Error: data at index: " << i << " does not match" << std::endl;
					for (const auto& res : data_._results)
					{
						std::cout << res << ','; 
					}
					std::cout << std::endl;
				}
			}
		}
		++dataProcessedNum;
	}};

	threads.emplace_back(dataTask, flags::task3End, flags::task4Start, flags::idle, finalizerTask);

	size_t secondsToWait{10};
	std::this_thread::sleep_for(std::chrono::seconds{secondsToWait});

	endProducer.store(true, std::memory_order_release);
	dataProducerThread.join();

	endProcessors.store(true, std::memory_order_release);
	for (auto& t : threads)
	{
		t.join();
	}

	std::cout << "pipeline processed: " << dataProcessedNum << " in " << secondsToWait << " seconds" << std::endl;


	// do the same processing in just 2 threads , producer and processor
	for (auto& d : queue)
	{
		d._flags = flags::idle;
	}
	dataProcessedNum = 0;

	// data producer thread
	endProducer.store(false, std::memory_order_release);
	auto dataProducerThread2{std::thread{dataProducerTask, flags::idle, flags::producerStart, flags::producerEnd, [](DataToProcess& data_){ data_ = DataToProcess{};}}};

	endProcessors.store(false, std::memory_order_release);
	auto dataProcessorThread{std::thread{dataTask, flags::producerEnd, flags::task1End, flags::idle, [&task1, &task2, &task3, &finalizerTask](DataToProcess& data_){ 
		task1(data_);
 		task2(data_);
		task3(data_);
		finalizerTask(data_);
	}}};

	std::this_thread::sleep_for(std::chrono::seconds{secondsToWait});

	endProducer.store(true, std::memory_order_release);
	dataProducerThread2.join();

	endProcessors.store(true, std::memory_order_release);
	dataProcessorThread.join();

	std::cout << "normal processed: " << dataProcessedNum << " in " << secondsToWait << " seconds" << std::endl;



/*
	tasks use CPU:
	---------------------------------------------------------------------------------
	auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 1000000; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	pipeline processed: 1433 in 10 seconds
	normal processed: 679 in 10 seconds

	auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 100000; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	pipeline processed: 12161 in 10 seconds
	normal processed: 4812 in 10 seconds

	auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 10000; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	pipeline processed: 180475 in 10 seconds
	normal processed: 67039 in 10 seconds

	auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 1000; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	pipeline processed: 3799496 in 10 seconds
	normal processed: 2010901 in 10 seconds

	auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 100; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	pipeline processed: 12677722 in 10 seconds
	normal processed: 11034431 in 10 seconds

	auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 10; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	pipeline processed: 17477984 in 10 seconds
	normal processed: 20366163 in 10 seconds

	auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 0; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	pipeline processed: 17416437 in 10 seconds
	normal processed: 23858330 in 10 seconds

	tasks sleep:
	---------------------------------------------------------------------------------
	auto waistTime{[](){ std::this_thread::sleep_for(std::chrono::microseconds{10});}};
	pipeline processed: 21201 in 10 seconds
	normal processed: 7798 in 10 seconds

	auto waistTime{[](){ std::this_thread::sleep_for(std::chrono::microseconds{1});}};
	pipeline processed: 103213 in 10 seconds
	normal processed: 252551 in 10 seconds
*/
}

void testPipeline()
{
	struct DataToProcess
	{
		std::vector<std::string> _results; // every task appends some result string
	};

	//auto waistTime{[](){ double res{0}; for (int i = 0 ; i < 0; ++i){res += sqrt(static_cast<double>(i * i * i) / 100.0);} return res;}};
	auto waistTime{[](){ std::this_thread::sleep_for(std::chrono::microseconds{10});}};
	//auto waistTime{[](){ }};
	auto producerTask{[](DataToProcess& data_){ data_ = DataToProcess{};}};
	auto processorTask1{[&waistTime](DataToProcess& data_){data_._results.emplace_back("task1 worked"); waistTime();}};
	auto processorTask2{[&waistTime](DataToProcess& data_){data_._results.emplace_back("task2 worked"); waistTime();}};
	auto processorTask3{[&waistTime](DataToProcess& data_){data_._results.emplace_back("task3 worked"); waistTime();}};

	size_t dataProcessedNum{0};
	auto finalizerTask{[&dataProcessedNum](DataToProcess& data_){
		data_._results.emplace_back("finished processing");

		const std::vector<std::string> expectedResults{"task1 worked", "task2 worked", "task3 worked", "finished processing"};

		if (expectedResults.size() != data_._results.size())
		{
			std::cout << "Error: data size: " << data_._results.size() << " does not match, expected: " << expectedResults.size() << std::endl;
			for (const auto& res : data_._results)
			{
				std::cout << res << ','; 
			}
			std::cout << std::endl;
		}
		else
		{
			for (size_t i = 0 ; i < expectedResults.size() ; ++i)
			{
				if (expectedResults[i] != data_._results[i])
				{
					std::cout << "Error: data at index: " << i << " does not match" << std::endl;
					for (const auto& res : data_._results)
					{
						std::cout << res << ','; 
					}
					std::cout << std::endl;
				}
			}
		}
		++dataProcessedNum;
	}};

	pipeLine<128, DataToProcess> pl;
	pl.addProducer(producerTask);
	pl.addProcessor(processorTask1);
	pl.addProcessor(processorTask2);
	pl.addProcessor(processorTask3);
	pl.addFinalizer(finalizerTask);

	size_t secondsToWait{10};

	std::cout << "start pipeline for : " << secondsToWait << " seconds" << std::endl;
	int repeat{10};
	while(repeat-- > 0)
	{
		dataProcessedNum = 0;
		pl.start();
		for (size_t i = 0 ; i < secondsToWait ; ++i)
		{
			std::cout << '.' << std::flush;
			std::this_thread::sleep_for(std::chrono::seconds{1});
		}
		pl.stop();
		std::cout << "pipeline processed: " << dataProcessedNum << " in " << secondsToWait << " seconds" << std::endl;
	}
	
	std::cout << "start normal processing for : " << secondsToWait << " seconds" << std::endl;
	repeat = 10;
	while(repeat-- > 0)
	{
		dataProcessedNum = 0;
		std::atomic<bool> end{false};
		std::thread notPipelineThread{[&end, &producerTask, &processorTask1, &processorTask2, &processorTask3, finalizerTask](){
			DataToProcess data;
			while (!end.load(std::memory_order_acquire))
			{
				producerTask(data);
				processorTask1(data);
				processorTask2(data);
				processorTask3(data);
				finalizerTask(data);
			}
		}};

		for (size_t i = 0 ; i < secondsToWait ; ++i)
		{
			std::cout << '.' << std::flush;
			std::this_thread::sleep_for(std::chrono::seconds{1});
		}
		end.store(true, std::memory_order_release);
		notPipelineThread.join();
		std::cout << "normal processed: " << dataProcessedNum << " in " << secondsToWait << " seconds" << std::endl;
	}

/*
auto waistTime{[](){ std::this_thread::sleep_for(std::chrono::microseconds{10});}};

start pipeline for : 10 seconds
..........pipeline processed: 22420 in 10 seconds
..........pipeline processed: 22602 in 10 seconds
..........pipeline processed: 22423 in 10 seconds
..........pipeline processed: 22192 in 10 seconds
..........pipeline processed: 22401 in 10 seconds
..........pipeline processed: 22512 in 10 seconds
..........pipeline processed: 22362 in 10 seconds
..........pipeline processed: 29185 in 10 seconds
..........pipeline processed: 22086 in 10 seconds
..........pipeline processed: 22826 in 10 seconds
start normal processing for : 10 seconds
..........normal processed: 11067 in 10 seconds
..........normal processed: 12065 in 10 seconds
..........normal processed: 11353 in 10 seconds
..........normal processed: 11376 in 10 seconds
..........normal processed: 10389 in 10 seconds
..........normal processed: 10632 in 10 seconds
..........normal processed: 9792 in 10 seconds
..........normal processed: 10276 in 10 seconds
..........normal processed: 10232 in 10 seconds
..........normal processed: 9995 in 10 seconds

*/	
}

int main(int /*argc*/, char* /*argv*/[])
{
    //testAtomicFlag();
	//std::cout << "---------------------------------------------" << std::endl;
	//testCompareExchange();
	//std::cout << "---------------------------------------------" << std::endl;
	//testQueueProcessing();
	testPipeline();
    return 0;
}