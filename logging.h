// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>

#include "config/config.h"
#include "utils.h"

using namespace config;

enum class LogLevel { LOG_NONE = 0, LOG_INFO = 1, LOG_ERROR = 2 };

#if defined(DEBUG_LOG) || defined(ENABLE_STATISTICS)
inline std::unique_ptr<std::ofstream> log_file_stream = nullptr;

inline void CreateLogFile(const char* file_name) {
  log_file_stream = std::make_unique<std::ofstream>(file_name, std::ios::app);
}

inline void CloseLogFile() { log_file_stream.reset(); }

inline std::ostream& GetLogStream() {
  if (log_file_stream && log_file_stream->is_open()) {
    return *log_file_stream;
  }
  return std::cout;
}

inline void LogImpl(std::ostream& stream) { stream << std::endl; }

template <typename T, typename... Args>
inline void LogImpl(std::ostream& stream, T&& first, Args&&... rest) {
  stream << std::forward<T>(first);
  LogImpl(stream, std::forward<Args>(rest)...);
}
#endif

#ifdef DEBUG_LOG
template <typename... Args>
inline void Log(LogLevel level, Args&&... args) {
  if (static_cast<uint32_t>(level) < configs[LOG_LEVEL]) {
    return;
  }
  auto& stream = GetLogStream();
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
  LogImpl(stream, std::forward<Args>(args)...);
}
#else
template <typename... Args>
inline void Log(LogLevel, Args&&...) {}
#endif

#ifdef ENABLE_STATISTICS
template <typename... Args>
inline void LogStats(Args&&... args) {
  auto& stream = GetLogStream();
  stream << "Stats:\n";
  LogImpl(stream, std::forward<Args>(args)...);
}
#else
template <typename... Args>
inline void LogStats(Args&&...) {}
#endif

#ifdef DEBUG_LOG
template <typename... Args>
inline void PrintDeflateBlockHeader(LogLevel level, uint8_t* data, uint32_t len,
                                    int window_bits, Args&&... args) {
  if (static_cast<uint32_t>(level) < configs[LOG_LEVEL]) {
    return;
  }
  CompressedFormat format = GetCompressedFormat(window_bits);
  uint32_t header_length = GetHeaderLength(format);
  if (len >= (header_length + 1)) {
    Log(level, "Deflate block header bfinal=",
        static_cast<int>(data[header_length] & 0b00000001),
        ", btype=", static_cast<int>((data[header_length] & 0b00000110) >> 1),
        "\n", std::forward<Args>(args)...);
  }
}
#else
template <typename... Args>
inline void PrintDeflateBlockHeader(LogLevel, uint8_t*, uint32_t, int,
                                    Args&&...) {}
#endif
