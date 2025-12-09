// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <string>

#include "../zlib_accel.h"

#ifdef DEBUG_LOG
void Log(std::string message);
#else
#define Log(...)
#endif

int ZlibCompress(const char* input, size_t input_length, std::string* output,
                 int window_bits, int flush, size_t* output_upper_bound,
                 ExecutionPath* execution_path);

int ZlibUncompress(const char* input, size_t input_length, size_t output_length,
                   char** uncompressed, size_t* uncompressed_length,
                   size_t* input_consumed, int window_bits, int flush,
                   int input_chunks, ExecutionPath* execution_path);
