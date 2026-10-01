# Copyright (C) 2025 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.10)

project (zlib-accel
        VERSION 1.1.0
        DESCRIPTION "Zlib shim to accelerate deflate/inflate")

include(common.cmake)
link_libraries(dl)

option(ENABLE_STATISTICS "Enable statistics" OFF)
if(ENABLE_STATISTICS)
  add_compile_definitions(ENABLE_STATISTICS)
endif()

add_library(${PROJECT_NAME} SHARED config/config_reader.cpp config/config.cpp zlib_accel.cpp iaa.cpp qat.cpp utils.cpp statistics.cpp igzip.cpp)

# The shim exports zlib's own symbols, so a consumer that links it -- or preloads
# one build against a binary built for another -- has no other way to tell two
# incompatible versions apart. Carry a soname so the major version is part of the
# library's identity; the unversioned libzlib-accel.so symlink keeps LD_PRELOAD
# and the test and fuzzing links working unchanged.
set_target_properties(${PROJECT_NAME} PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})

add_custom_target(format
    find .. -iname '*.h' -o -iname '*.cpp' | xargs clang-format -style=Google -i
)
