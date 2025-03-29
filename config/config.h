// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

#include <string>

namespace config {
enum ConfigOption {
  USE_QAT_COMPRESS,
  USE_QAT_UNCOMPRESS,
  USE_IAA_COMPRESS,
  USE_IAA_UNCOMPRESS,
  USE_ZLIB_COMPRESS,
  USE_ZLIB_UNCOMPRESS,
  IAA_COMPRESS_PERCENTAGE,
  IAA_DECOMPRESS_PERCENTAGE,
  IAA_PREPEND_EMPTY_BLOCK,
  QAT_PERIODICAL_POLLING,
  QAT_COMPRESSION_LEVEL,
  LOG_LEVEL
};

extern int use_qat_compress;
extern int use_qat_uncompress;
extern int use_iaa_compress;
extern int use_iaa_uncompress;
extern int use_zlib_compress;
extern int use_zlib_uncompress;
extern int iaa_compress_percentage;
extern int iaa_decompress_percentage;
extern int iaa_prepend_empty_block;
extern int qat_periodical_polling;
extern int qat_compression_level;
extern std::string log_file;
extern int log_level;

VISIBLE_FOR_TESTING bool LoadConfigFile(
    std::string& file_content, const char* filePath = "/etc/zlib-accel.conf");

VISIBLE_FOR_TESTING void SetConfig(ConfigOption option, int value);
VISIBLE_FOR_TESTING int GetConfig(ConfigOption option);
}  // namespace config
