// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include "config_reader.h"

namespace config {

std::string log_file = "";

// default config values initialization
uint32_t configs[CONFIG_MAX] = {
    1,   /*use_qat_compress*/
    1,   /*use_qat_uncompress*/
    0,   /*use_iaa_compress*/
    0,   /*use_iaa_uncompress*/
    1,   /*use_zlib_compress*/
    1,   /*use_zlib_uncompress*/
    50,  /*iaa_compress_percentage*/
    50,  /*iaa_uncompress_percentage*/
    0,   /*iaa_prepend_empty_block*/
    0,   /*qat_periodical_polling*/
    1,   /*qat_compression_level*/
    0,   /*qat_compression_allow_chunking*/
    0,   /*ignore_zlib_dictionary*/
    2,   /*log_level*/
    1000 /*log_stats_samples*/
};

bool LoadConfigFile(std::string& file_content, const char* file_path) {
  // Initialize config_names within the function to avoid initialization order
  // problems. LoadConfigFile is called from the zlib-accel shared library
  // constructor. If config_names is a global array of strings, it may not be
  // initialized yet when the constructor is executed.
  // clang-format off
  static const std::string config_names[CONFIG_MAX] {
    "use_qat_compress",
    "use_qat_uncompress",
    "use_iaa_compress",
    "use_iaa_uncompress",
    "use_zlib_compress",
    "use_zlib_uncompress",
    "iaa_compress_percentage",
    "iaa_uncompress_percentage",
    "iaa_prepend_empty_block",
    "qat_periodical_polling",
    "qat_compression_level",
	  "qat_compression_allow_chunking",
    "ignore_zlib_dictionary",
    "log_level",
    "log_stats_samples"
  };
  // clang-format on

  const bool exists = std::filesystem::exists(file_path);
  const bool symlink = std::filesystem::is_symlink(file_path);
  if (!exists || symlink) {
    return false;
  }
  ConfigReader config_reader;
  config_reader.ParseFile(file_path);

  auto trySetConfig = [&](ConfigOption opt, uint32_t max, uint32_t min) {
    uint32_t value;
    if (config_reader.GetValue(config_names[opt], value, max, min)) {
      configs[opt] = value;
    }
  };

  trySetConfig(USE_QAT_COMPRESS, 1, 0);
  trySetConfig(USE_QAT_UNCOMPRESS, 1, 0);
  trySetConfig(USE_IAA_COMPRESS, 1, 0);
  trySetConfig(USE_IAA_UNCOMPRESS, 1, 0);
  trySetConfig(USE_ZLIB_COMPRESS, 1, 0);
  trySetConfig(USE_ZLIB_UNCOMPRESS, 1, 0);
  trySetConfig(IAA_COMPRESS_PERCENTAGE, 100, 0);
  trySetConfig(IAA_UNCOMPRESS_PERCENTAGE, 100, 0);
  trySetConfig(IAA_PREPEND_EMPTY_BLOCK, 1, 0);
  trySetConfig(QAT_PERIODICAL_POLLING, 1, 0);
  trySetConfig(QAT_COMPRESSION_LEVEL, 9, 1);
  trySetConfig(QAT_COMPRESSION_ALLOW_CHUNKING, 1, 0);
  trySetConfig(IGNORE_ZLIB_DICTIONARY, 1, 0);
  trySetConfig(LOG_LEVEL, 2, 0);
  trySetConfig(LOG_STATS_SAMPLES, UINT32_MAX, 0);

  config_reader.GetValue("log_file", log_file);
  file_content.append(config_reader.DumpValues());

  return true;
}

void SetConfig(ConfigOption option, uint32_t value) { configs[option] = value; }

uint32_t GetConfig(ConfigOption option) { return configs[option]; }

}  // namespace config
