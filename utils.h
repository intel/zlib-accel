// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

inline constexpr uint32_t GZIP_EXT_XHDR_SIZE = 14;
inline constexpr uint32_t GZIP_EXT_HDRFTR_SIZE =
    32;  // size of header + footer for gzip ext format

enum class CompressedFormat { DEFLATE_RAW, ZLIB, GZIP, INVALID };

CompressedFormat GetCompressedFormat(int window_bits);
int GetTrailerLength(CompressedFormat format);
int GetHeaderLength(CompressedFormat format, bool gzip_ext = false);
int GetWindowSizeFromZlibHeader(uint8_t* data, uint32_t len);
bool DetectGzipExt(uint8_t* data, uint32_t len, uint32_t* src_size,
                   uint32_t* dest_size);
