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

add_executable(zlib_accel_test
    zlib_accel_test.cpp
    logging_test.cpp
    test_utils.cpp
    ../utils.cpp
)

add_custom_target(run
    COMMAND ./zlib_accel_test
    DEPENDS zlib_accel_test
)

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
