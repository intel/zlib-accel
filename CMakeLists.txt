# Copyright (C) 2025 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.10)

project (zlib-accel
        VERSION 0.1
        DESCRIPTION "Zlib shim to accelerate deflate/inflate")

include(common.cmake)

add_library(${PROJECT_NAME} SHARED config/config_reader.cpp config/config.cpp zlib_accel.cpp iaa.cpp qat.cpp utils.cpp sharded_map.h)

add_custom_target(format
    find .. -iname '*.h' -o -iname '*.cpp' | xargs clang-format -style=Google -i
)
