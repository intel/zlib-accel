// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "utils.h"

#include <cstdint>

CompressedFormat GetCompressedFormat(int window_bits) {
  if (window_bits >= -15 && window_bits <= -8) {
    return CompressedFormat::DEFLATE_RAW;
  } else if (window_bits >= 8 && window_bits <= 15) {
    return CompressedFormat::ZLIB;
  } else if (window_bits >= 24 && window_bits <= 31) {
    return CompressedFormat::GZIP;
  }
  return CompressedFormat::INVALID;
}

int GetTrailerLength(CompressedFormat format) {
  switch (format) {
    case CompressedFormat::ZLIB:
      return 4;
    case CompressedFormat::GZIP:
      return 8;
    default:
      return 0;
  }
}

int GetHeaderLength(CompressedFormat format, bool gzip_ext) {
  switch (format) {
    case CompressedFormat::ZLIB:
      return 2;
    case CompressedFormat::GZIP:
      if (gzip_ext) {
        return 24;
      } else {
        return 10;
      }
    default:
      return 0;
  }
}

int GetWindowSizeFromZlibHeader(uint8_t* data, uint32_t len) {
  if (len > 0) {
    return (data[0] >> 4) + 8;
  }
  // Default is to assume max window size (32kB)
  return 15;
}

bool DetectGzipExt(uint8_t* data, uint32_t len, uint32_t* src_size,
                   uint32_t* dest_size) {
  // Standard header
  // ID1: 31
  // ID2: 139
  // CM: 8
  // FLG: bit 2 is FEXTRA

  // After standard 10-byte header:
  // XLEN (2B): 12
  // SI1 (1B): 'Q'
  // SI2 (1B): 'Z'
  // Length of subheader (2B): 8
  // src size (4B)
  // dest size (4B)

  if (len < 24) {
    return false;
  }

  // Check beginning of standard header
  if (data[0] != 31 || data[1] != 139 || data[2] != 8) {
    return false;
  }
  // Check FLG.FEXTRA
  uint8_t fextra = (data[3] & 0x4) >> 2;
  if (fextra != 1) {
    return false;
  }
  // Check extended header
  if (data[10] != 12 || data[11] != 0 || data[12] != 'Q' || data[13] != 'Z' ||
      data[14] != 8 || data[15] != 0) {
    return false;
  }

  // Extract sizes from extended header
  *src_size = *(reinterpret_cast<uint32_t*>(data + 16));
  *dest_size = *(reinterpret_cast<uint32_t*>(data + 20));
  return true;
}
