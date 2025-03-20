// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace config {
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

bool LoadConfigFile(std::string& file_content,
                    const char* filePath = "/etc/zlib-accel.conf");
}  // namespace config
