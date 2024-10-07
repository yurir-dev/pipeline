cmake_minimum_required(VERSION 3.10)

set (PROJECT_NAME "lockFreeQueue")
set (EXE_NAME "lockFreeQueue")

# set the project name
project(${PROJECT_NAME})

if(MSVC)
    add_compile_options("/W4" "/WX" "$<$<CONFIG:RELEASE>:/O2>")
else()
    add_compile_options("-Wall" "-Wextra" "-Werror" "-g" "-O2" "$<$<CONFIG:RELEASE>:-O3>")
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        add_compile_options("-stdlib=libc++")
    else()
        # nothing special for gcc at the moment
    endif()
endif()

set (SOURCES main.cpp pipeline.h)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# add the executable
add_executable(${EXE_NAME} ${SOURCES})

enable_testing()
add_subdirectory(tests)