// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>

#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

enum class Statistic : size_t {
  DEFLATE_COUNT = 0,
  DEFLATE_ERROR_COUNT,
  DEFLATE_QAT_COUNT,
  DEFLATE_QAT_ERROR_COUNT,
  DEFLATE_IAA_COUNT,
  DEFLATE_IAA_ERROR_COUNT,
  DEFLATE_ZLIB_COUNT,
  INFLATE_COUNT,
  INFLATE_ERROR_COUNT,
  INFLATE_QAT_COUNT,
  INFLATE_QAT_ERROR_COUNT,
  INFLATE_IAA_COUNT,
  INFLATE_IAA_ERROR_COUNT,
  INFLATE_ZLIB_COUNT,
  STATS_COUNT
};

constexpr size_t STATS_COUNT = static_cast<size_t>(Statistic::STATS_COUNT);

inline std::ostream& operator<<(std::ostream& os, const Statistic& stat) {
  return os << static_cast<size_t>(stat);
}

#ifdef ENABLE_STATISTICS
#define INCREMENT_STAT(stat) stats[static_cast<size_t>(Statistic::stat)]++
#define INCREMENT_STAT_COND(cond, stat) \
  if (cond) stats[static_cast<size_t>(Statistic::stat)]++
#else
#define INCREMENT_STAT(stat)
#define INCREMENT_STAT_COND(cond, stat)
#endif

#ifdef ENABLE_STATISTICS
extern thread_local std::array<uint64_t, STATS_COUNT> stats;
extern const std::array<const char*, STATS_COUNT> stat_names;
#endif

VISIBLE_FOR_TESTING constexpr bool AreStatsEnabled() {
#ifdef ENABLE_STATISTICS
  return true;
#else
  return false;
#endif
}

VISIBLE_FOR_TESTING inline void ResetStats() {
#ifdef ENABLE_STATISTICS
  stats.fill(0);
#endif
}

VISIBLE_FOR_TESTING inline uint64_t GetStat(Statistic stat) {
#ifdef ENABLE_STATISTICS
  return stats[static_cast<size_t>(stat)];
#else
  (void)stat;
  return 0;
#endif
}

#ifdef ENABLE_STATISTICS
void PrintStats();
#else
#define PrintStats(...)
#endif
