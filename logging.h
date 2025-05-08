// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/config.h"
#include "utils.h"
using namespace config;
enum class LogLevel { LOG_NONE = 0, LOG_INFO = 1, LOG_ERROR = 2 };

#if defined(DEBUG_LOG) || defined(ENABLE_STATISTICS)
inline FILE* log_file_stream = nullptr;

inline static void CreateLogFile(const char* file_name) {
  log_file_stream = fopen(file_name, "w");
}

inline static void CloseLogFile() {
  if (log_file_stream != nullptr) {
    fclose(log_file_stream);
  }
}
#endif

#ifdef DEBUG_LOG
static inline void Log(LogLevel level, const char* format, ...) {
  if (static_cast<int>(level) < configs[LOG_LEVEL]) {
    return;
  }

  FILE* stream = stdout;
  if (log_file_stream != nullptr) {
    stream = log_file_stream;
  }

  switch (level) {
    case LogLevel::LOG_ERROR:
      fprintf(stream, "Error: ");
      break;
    case LogLevel::LOG_INFO:
      fprintf(stream, "Info: ");
      break;
    case LogLevel::LOG_NONE:
      return;
  }
  va_list args;
  va_start(args, format);
  vfprintf(stream, format, args);
  va_end(args);
}
#else
#define Log(...)
#endif

#ifdef ENABLE_STATISTICS
static inline void LogStats(const char* stats_str) {
  FILE* stream = stdout;
  if (log_file_stream != nullptr) {
    stream = log_file_stream;
  }

  fprintf(stream, "Stats:\n");
  fprintf(stream, "%s", stats_str);
}
#else
#define LogStats(...)
#endif

#ifdef DEBUG_LOG
static inline void PrintDeflateBlockHeader(LogLevel level, uint8_t* data,
                                           uint32_t len, int window_bits) {
  if (static_cast<int>(level) < configs[LOG_LEVEL]) {
    return;
  }

  CompressedFormat format = GetCompressedFormat(window_bits);
  uint32_t header_length = GetHeaderLength(format);
  if (len >= (header_length + 1)) {
    Log(level, "Deflate block header bfinal=%d, btype=%d\n",
        data[header_length] & 0b00000001,
        (data[header_length] & 0b00000110) >> 1);
  }
}
#else
#define PrintDeflateBlockHeader(...)
#endif
