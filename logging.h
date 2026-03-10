// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>

#include "config/config.h"
#include "utils.h"

using namespace config;

enum class LogLevel { LOG_NONE = 0, LOG_INFO = 1, LOG_ERROR = 2 };

#if defined(DEBUG_LOG) || defined(ENABLE_STATISTICS)

inline std::unique_ptr<std::ofstream>& LogFileStream() {
  static std::unique_ptr<std::ofstream> stream;
  return stream;
}

inline void CreateLogFile(const char* file_name) {
  auto& s = LogFileStream();
  s = std::make_unique<std::ofstream>(file_name, std::ios::app);
}

inline void CloseLogFile() {
  auto& s = LogFileStream();
  s.reset();
}

inline std::ostream& GetLogStream() {
  auto& s = LogFileStream();
  if (s && s->is_open()) {
    return *s;
  }
  return std::cout;
}

#endif  // DEBUG_LOG || ENABLE_STATISTICS

#ifdef DEBUG_LOG

static std::mutex log_mutex;
template <typename... Args>
inline void Log(LogLevel level, Args&&... args) {
  std::lock_guard<std::mutex> lock(log_mutex);
  uint32_t current_level = config::GetConfig(config::LOG_LEVEL);

  if (current_level == static_cast<uint32_t>(LogLevel::LOG_NONE)) {
    return;
  }

  if (static_cast<uint32_t>(level) < current_level) {
    return;
  }

  std::ostream& stream = GetLogStream();
  stream << std::dec;
  switch (level) {
    case LogLevel::LOG_ERROR:
      stream << "Error: ";
      break;
    case LogLevel::LOG_INFO:
      stream << "Info: ";
      break;
    case LogLevel::LOG_NONE:
      return;
  }
  (..., (stream << args));
  stream << std::flush;
}

#else
#define Log(...)
#endif

#ifdef ENABLE_STATISTICS

template <typename... Args>
inline void LogStats(Args&&... args) {
  std::ostream& stream = GetLogStream();
  stream << "Stats:\n";
  (..., (stream << args));
  stream << std::flush;
}

#else
#define LogStats(...)
#endif

#ifdef DEBUG_LOG

template <typename... Args>
inline void PrintDeflateBlockHeader(LogLevel level, uint8_t* data, uint32_t len,
                                    int window_bits, Args&&... args) {
  uint32_t current_level = config::GetConfig(config::LOG_LEVEL);

  if (current_level == static_cast<uint32_t>(LogLevel::LOG_NONE)) {
    return;
  }

  if (static_cast<uint32_t>(level) < current_level) {
    return;
  }

  CompressedFormat format = GetCompressedFormat(window_bits);
  uint32_t header_length = GetHeaderLength(format);
  if (len >= (header_length + 1)) {
    Log(level, "Deflate block header bfinal = ",
        static_cast<int>(data[header_length] & 0b00000001),
        ", btype = ", static_cast<int>((data[header_length] & 0b00000110) >> 1),
        "\n", std::forward<Args>(args)...);
  }
}

#else
#define PrintDeflateBlockHeader(...)
#endif
