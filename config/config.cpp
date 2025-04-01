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
  const bool symlink = std::filesystem::is_symlink(filePath);
  if (!exists || symlink) {
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

void SetConfig(ConfigOption option, int value) {
  switch (option) {
    case USE_QAT_COMPRESS:
      use_qat_compress = value;
      break;
    case USE_QAT_UNCOMPRESS:
      use_qat_uncompress = value;
      break;
    case USE_IAA_COMPRESS:
      use_iaa_compress = value;
      break;
    case USE_IAA_UNCOMPRESS:
      use_iaa_uncompress = value;
      break;
    case USE_ZLIB_COMPRESS:
      use_zlib_compress = value;
      break;
    case USE_ZLIB_UNCOMPRESS:
      use_zlib_uncompress = value;
      break;
    case IAA_COMPRESS_PERCENTAGE:
      iaa_compress_percentage = value;
      break;
    case IAA_DECOMPRESS_PERCENTAGE:
      iaa_decompress_percentage = value;
      break;
    case IAA_PREPEND_EMPTY_BLOCK:
      iaa_prepend_empty_block = value;
      break;
    case QAT_PERIODICAL_POLLING:
      qat_periodical_polling = value;
      break;
    case QAT_COMPRESSION_LEVEL:
      qat_compression_level = value;
      break;
    case LOG_LEVEL:
      log_level = value;
      break;
  }
}

int GetConfig(ConfigOption option) {
  switch (option) {
    case USE_QAT_COMPRESS:
      return use_qat_compress;
    case USE_QAT_UNCOMPRESS:
      return use_qat_uncompress;
    case USE_IAA_COMPRESS:
      return use_iaa_compress;
    case USE_IAA_UNCOMPRESS:
      return use_iaa_uncompress;
    case USE_ZLIB_COMPRESS:
      return use_zlib_compress;
    case USE_ZLIB_UNCOMPRESS:
      return use_zlib_uncompress;
    case IAA_COMPRESS_PERCENTAGE:
      return iaa_compress_percentage;
    case IAA_DECOMPRESS_PERCENTAGE:
      return iaa_decompress_percentage;
    case IAA_PREPEND_EMPTY_BLOCK:
      return iaa_prepend_empty_block;
    case QAT_PERIODICAL_POLLING:
      return qat_periodical_polling;
    case QAT_COMPRESSION_LEVEL:
      return qat_compression_level;
    case LOG_LEVEL:
      return log_level;
  }
  return 0;
}
}  // namespace config
