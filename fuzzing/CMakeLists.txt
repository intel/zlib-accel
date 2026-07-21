# Copyright (C) 2025 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.15)

project(zlib-accel-test VERSION 1.0.0)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "Fuzz testing requires the Clang compiler.")
endif()

link_directories(../build)
link_libraries(zlib-accel)

include(../common.cmake)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fsanitize=address,fuzzer")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fsanitize=address,fuzzer")

add_executable(zlib_accel_fuzz zlib_accel_fuzz.cpp ../tests/test_utils.cpp ../utils.cpp)

add_custom_target(run
    COMMAND ./zlib_accel_fuzz
    DEPENDS zlib_accel_fuzz
)
