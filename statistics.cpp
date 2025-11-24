// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "statistics.h"

#ifdef ENABLE_STATISTICS

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "config/config.h"
#include "logging.h"

using namespace config;

const std::array<const char*, STATS_COUNT> stat_names{
    {"deflate_count", "deflate_error_count", "deflate_qat_count",
     "deflate_qat_error_count", "deflate_iaa_count", "deflate_iaa_error_count",
     "deflate_zlib_count", "inflate_count", "inflate_error_count",
     "inflate_qat_count", "inflate_qat_error_count", "inflate_iaa_count",
     "inflate_iaa_error_count", "inflate_zlib_count"}};

thread_local std::array<uint64_t, STATS_COUNT> stats{};

void PrintStats() {
  auto total_operations = stats[static_cast<size_t>(Statistic::DEFLATE_COUNT)] +
                          stats[static_cast<size_t>(Statistic::INFLATE_COUNT)];
  if (total_operations % configs[LOG_STATS_SAMPLES] != 0) {
    return;
  }
  LogStats("Thread: ", std::this_thread::get_id(), "\n");
  for (size_t i = 0; i < stats.size(); ++i) {
    LogStats(stat_names[i], " = ", stats[i], "\n");
  }
}
#endif
