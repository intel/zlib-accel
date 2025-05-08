// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include <climits>
#include <filesystem>

#include "../logging.h"
#include "config_reader.h"

using namespace std;

namespace config {

std::string log_file = "";

const std::string config_names[CONFIG_MAX]{
    "use_qat_compress",        "use_qat_uncompress",
    "use_iaa_compress",        "use_iaa_uncompress",
    "use_zlib_compress",       "USE_ZLIB_UNCOMPRESS",
    "use_zlib_uncompress",     "iaa_compress_percentage",
    "iaa_prepend_empty_block", "qat_periodical_polling",
    "qat_compression_level",   "log_level",
    "log_stats_samples"};

uint16_t configs[CONFIG_MAX] = {1, 1, 0, 0, 1, 1, 50, 50, 0, 0, 1, 2, 1000};


bool LoadConfigFile(std::string& file_content, const char* filePath) {
  const bool exists = std::filesystem::exists(filePath);
  const bool symlink = std::filesystem::is_symlink(filePath);
  if (!exists || symlink) {
    return false;
  }
  ConfigReader configReader;
  configReader.ParseFile(filePath);
  int value = 0;
  configReader.GetValue(config_names[USE_QAT_COMPRESS], value, 1, 0);
  configs[USE_QAT_COMPRESS] = value;
  configReader.GetValue(config_names[USE_QAT_UNCOMPRESS], value, 1, 0);
  configs[USE_QAT_UNCOMPRESS] = value;
  configReader.GetValue(config_names[USE_IAA_COMPRESS], value, 1, 0);
  configs[USE_IAA_COMPRESS] = value;
  configReader.GetValue(config_names[USE_IAA_UNCOMPRESS], value, 1, 0);
  configs[USE_IAA_UNCOMPRESS] = value;
  configReader.GetValue(config_names[USE_ZLIB_COMPRESS], value, 1, 0);
  configs[USE_ZLIB_COMPRESS] = value;
  configReader.GetValue(config_names[USE_ZLIB_UNCOMPRESS], value, 1, 0);
  configs[USE_ZLIB_UNCOMPRESS] = value;
  configReader.GetValue(config_names[IAA_COMPRESS_PERCENTAGE], value, 100, 0);
  configs[IAA_COMPRESS_PERCENTAGE] = value;
  configReader.GetValue(config_names[IAA_UNCOMPRESS_PERCENTAGE], value, 100, 0);
  configs[IAA_UNCOMPRESS_PERCENTAGE] = value;
  configReader.GetValue(config_names[IAA_PREPEND_EMPTY_BLOCK], value, 1, 0);
  configs[IAA_PREPEND_EMPTY_BLOCK] = value;
  configReader.GetValue(config_names[QAT_PERIODICAL_POLLING], value, 1, 0);
  configs[QAT_PERIODICAL_POLLING] = value;
  configReader.GetValue(config_names[QAT_COMPRESSION_LEVEL], value, 9, 1);
  configs[QAT_COMPRESSION_LEVEL] = value;
  configReader.GetValue(config_names[LOG_LEVEL], value, 2, 0);
  configs[LOG_LEVEL] = value;
  configReader.GetValue(config_names[LOG_STATS_SAMPLES], value, 1000, 0);
  configs[LOG_STATS_SAMPLES] = value;
  configReader.GetValue("log_file", log_file);
  file_content.append(configReader.DumpValues());

  return true;
}

void SetConfig(ConfigOption option, int value) { configs[option] = value; }

int GetConfig(ConfigOption option) { return configs[option]; }

}  // namespace config
