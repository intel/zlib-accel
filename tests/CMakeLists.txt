# Copyright (C) 2025 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.15)

project(zlib-accel-test VERSION 1.0.0)

link_directories(../build)
link_libraries(zlib-accel)

include(../common.cmake)

find_package(GTest REQUIRED)
link_libraries(gtest pthread)

# TODO Remove once crc16 references fixed in QPL
add_link_options("-Wl,--unresolved-symbols=ignore-all")

# This order is load-bearing. It fixes the order in which the suites register,
# hence the order they run in, hence where each test sits in the std::rand()
# sequence the whole binary shares -- and hence which payloads the parameterized
# cases get and which of them QATzip refuses. Moving a source here can change
# the refusal baseline without changing a line of test code.
add_executable(zlib_accel_test
    zlib_accel_test.cpp
    logging_test.cpp
    inflate_test.cpp
    deflate_test.cpp
    gz_test.cpp
    midstream_test.cpp
    test_utils.cpp
    ../utils.cpp
)

if(ASAN)
    target_sources(zlib_accel_test PRIVATE asan_suppressions.cpp)
endif()

if(ASAN)
    # LeakSanitizer runs at exit under ASAN, and a vendor library holds
    # allocations it never releases; lsan.supp names them so the run can still
    # fail on a leak that is ours.
    add_custom_target(run
        COMMAND ${CMAKE_COMMAND} -E env
                LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp
                ./zlib_accel_test
        DEPENDS zlib_accel_test
    )
else()
    add_custom_target(run
        COMMAND ./zlib_accel_test
        DEPENDS zlib_accel_test
    )
endif()

if(COVERAGE)
    add_custom_target(coverage
        COMMAND lcov --directory . --capture --output-file zlib_accel_test.info
        COMMAND lcov --directory ../../build --capture --output-file zlib_accel.info
        COMMAND genhtml -o html zlib_accel_test.info zlib_accel.info
    )
else()
    add_custom_target(coverage
        COMMAND echo "Cannot generate coverage report. Build with COVERAGE=ON."
    )
endif()
