// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "../zlib_accel.h"

#ifdef DEBUG_LOG
void Log(std::string message);
#else
#define Log(...)
#endif

enum BlockCompressibilityType {
  compressible_block,
  incompressible_block,
  zero_block
};

char* GenerateBlock(size_t length, BlockCompressibilityType block_type);

// Deterministic counterpart to GenerateBlock() with the compressible_block
// profile, for a test that picks its own payload instead of taking one from a
// test parameter. GenerateBlock() draws from std::rand(), which nothing in this
// binary seeds and every test shares, so a new test that draws from it shifts
// the payload of every parameterized case that runs after it -- enough to move
// a borderline case onto a path it does not assert. Drawing from a caller-owned
// seed instead keeps a new test out of that sequence.
char* GenerateSeededCompressibleBlock(size_t length, uint32_t seed,
                                      int ratio = 4);

// Releases anything the suite hands out, so every producer here has to allocate
// the way this releases. It used to free() while ZlibUncompress() returned
// new[] memory, which ASAN halts on.
void DestroyBlock(char* buf);

void SetCompressPath(ExecutionPath path, bool zlib_fallback,
                     bool iaa_prepend_empty_block,
                     bool qat_compression_allow_chunking);

void SetUncompressPath(ExecutionPath path, bool zlib_fallback,
                       bool iaa_prepend_empty_block);

int ZlibCompressGzipFile(const char* input, size_t input_length);

int ZlibUncompressGzipFile(size_t output_length, char** uncompressed,
                           size_t* uncompressed_length);

int ZlibCompress(const char* input, size_t input_length, std::string* output,
                 int window_bits, int flush, size_t* output_upper_bound,
                 ExecutionPath* execution_path);

int ZlibUncompress(const char* input, size_t input_length, size_t output_length,
                   char** uncompressed, size_t* uncompressed_length,
                   size_t* input_consumed, int window_bits, int flush,
                   int input_chunks, ExecutionPath* execution_path);
