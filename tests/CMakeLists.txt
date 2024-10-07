cmake_minimum_required(VERSION 3.10)


include_directories(../.)
include_directories(./.)

#include_directories(${CMAKE_SOURCE_DIR} . ../ )

# Files common to all tests
set (COMMON_SOURCES ../pipeline.h)

set(TEST_PIPELINE test_pipeline)
add_executable(${TEST_PIPELINE} test_pipeline.cpp ${COMMON_SOURCES})


set(exes ${TEST_PIPELINE})

if (UNIX)
message("creating linux project")
foreach (exe IN LISTS exes)
	target_link_libraries(${exe} pthread)
endforeach()
endif()

foreach (exe IN LISTS exes)
	add_test(${exe} ${exe})
endforeach()
