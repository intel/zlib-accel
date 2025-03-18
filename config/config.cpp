// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include <filesystem>

#include "../logging.h"
#include "config_reader.h"

using namespace std;

namespace config {
int use_qat_compress = 1;
int use_qat_uncompress = 1;
int use_iaa_compress = 0;
int use_iaa_uncompress = 0;
int use_zlib_compress = 1;
int use_zlib_uncompress = 1;
int iaa_compress_percentage = 50;
int iaa_decompress_percentage = 50;
int iaa_prepend_empty_block = 0;
int qat_periodical_polling = 0;
int qat_compression_level = 1;
std::string log_file = "";
int log_level = 2;

bool LoadConfigFile(std::string& file_content, const char* filePath) {
  const bool exists = std::filesystem::exists(filePath);
  if (!exists) {
    return false;
  }
  ConfigReader configReader;
  configReader.ParseFile(filePath);

  configReader.GetValue("use_qat_compress", use_qat_compress, 1, 0);
  configReader.GetValue("use_qat_uncompress", use_qat_uncompress, 1, 0);
  configReader.GetValue("use_iaa_compress", use_iaa_compress, 1, 0);
  configReader.GetValue("use_iaa_uncompress", use_iaa_uncompress, 1, 0);
  configReader.GetValue("use_zlib_compress", use_zlib_compress, 1, 0);
  configReader.GetValue("use_zlib_uncompress", use_zlib_uncompress, 1, 0);
  configReader.GetValue("iaa_compress_percentage", iaa_compress_percentage, 100,
                        0);
  configReader.GetValue("iaa_decompress_percentage", iaa_decompress_percentage,
                        100, 0);
  configReader.GetValue("iaa_prepend_empty_block", iaa_prepend_empty_block, 1,
                        0);
  configReader.GetValue("qat_periodical_polling", qat_periodical_polling, 1, 0);
  configReader.GetValue("qat_compression_level", qat_compression_level, 9, 1);
  configReader.GetValue("log_file", log_file);
  configReader.GetValue("log_level", log_level, 2, 0);

  file_content.append(configReader.DumpValues());

  return true;
}
}  // namespace config
