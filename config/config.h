// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>

#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

namespace config {
enum ConfigOption {
  USE_QAT_COMPRESS,
  USE_QAT_UNCOMPRESS,
  USE_IAA_COMPRESS,
  USE_IAA_UNCOMPRESS,
  USE_ZLIB_COMPRESS,
  USE_ZLIB_UNCOMPRESS,
  USE_IGZIP_COMPRESS,
  USE_IGZIP_UNCOMPRESS,
  IAA_COMPRESS_PERCENTAGE,
  IAA_UNCOMPRESS_PERCENTAGE,
  IAA_PREPEND_EMPTY_BLOCK,  // DEPRECATED — see README for details
  QAT_PERIODICAL_POLLING,
  QAT_COMPRESSION_LEVEL,
  QAT_COMPRESSION_ALLOW_CHUNKING,
  IGNORE_ZLIB_DICTIONARY,
  IGZIP_FALLBACK,
  LOG_LEVEL,
  LOG_STATS_SAMPLES,
  MAP_SHARDS,
  CONFIG_MAX
};

extern uint32_t configs[CONFIG_MAX];

inline constexpr const char* kDefaultConfigPath = "/etc/zlib-accel.conf";

// log_file, when given, receives the log path the config file names, and is
// left untouched when the file names none. It is an out-parameter rather than a
// global: the shim opens the log file a few lines after loading the config and
// has no reader for the path afterwards, and a global written from the library
// constructor is written before its own initializer runs.
VISIBLE_FOR_TESTING bool LoadConfigFile(
    std::string& file_content, const char* file_path = kDefaultConfigPath,
    std::string* log_file = nullptr);

VISIBLE_FOR_TESTING void SetConfig(ConfigOption option, uint32_t value);
VISIBLE_FOR_TESTING uint32_t GetConfig(ConfigOption option);
}  // namespace config
