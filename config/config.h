// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

#include <stdint.h>

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
  IAA_UNCOMPRESS_PERCENTAGE,
  IAA_PREPEND_EMPTY_BLOCK,
  QAT_PERIODICAL_POLLING,
  QAT_COMPRESSION_LEVEL,
  LOG_LEVEL,
  LOG_STATS_SAMPLES,
  CONFIG_MAX
};

extern std::string log_file;

extern uint16_t configs[CONFIG_MAX];

VISIBLE_FOR_TESTING bool LoadConfigFile(
    std::string& file_content, const char* filePath = "/etc/zlib-accel.conf");

VISIBLE_FOR_TESTING void SetConfig(ConfigOption option, int value);
VISIBLE_FOR_TESTING int GetConfig(ConfigOption option);
}  // namespace config
