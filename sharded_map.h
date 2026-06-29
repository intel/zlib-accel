// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <oneapi/tbb/concurrent_hash_map.h>
#include <string>
#include <utility>

#include "config/config.h"

using namespace config;

template <typename Key, typename Value>
class ShardedMap {
 public:
  ShardedMap(void) {
    const auto num_shards = configs[MAP_SHARDS];
    pd_map_arr = std::make_unique<PaddedMapType[]>(num_shards);
  }

  ~ShardedMap(void) {
    pd_map_arr.reset();
  }

  auto Get(const Key& key) -> decltype(std::declval<Value>().get()) {
    const auto shard = GetShard(key);
    typename MapType::const_accessor acc;
    if (!pd_map_arr[shard].map.find(acc, key)) {
      return nullptr;
    }
    return acc->second.get();
  }

  void Set(const Key& key, Value&& value) {
    const auto shard = GetShard(key);
    typename MapType::accessor acc;
    if (!pd_map_arr[shard].map.find(acc, key)) {
      // Key doesn't exist - add entry first
      pd_map_arr[shard].map.insert(acc, key);
    }

    // Set or update value associated with key
    acc->second = std::move(value);
  }

  void Unset(const Key& key) {
    const auto shard = GetShard(key);
    pd_map_arr[shard].map.erase(key);
  }

 private:
  // Number of bytes in a cache line
  static constexpr std::size_t CACHE_LINE_SIZE = 64;

  // Fibonacci hash constant: value is (2^64 / phi) to spread bits uniformly,
  // where phi is golden ratio and 64 is machine word length - constant is
  // multiplied by hash and then shifted right by top bits to get shard index
  static constexpr std::size_t FIBONACCI_MULTIPLIER = 11400714819323198485ull;

  struct HashCompare {
    static auto hash(const Key& key) -> std::size_t {
      return std::hash<Key>{}(key);
    }

    static auto equal(const Key& lhs, const Key& rhs) -> bool {
      return lhs == rhs;
    }
  };

  using MapType = oneapi::tbb::concurrent_hash_map<Key, Value, HashCompare>;
  struct alignas(CACHE_LINE_SIZE) PaddedMapType {
    MapType map;
  };
  std::unique_ptr<PaddedMapType[]> pd_map_arr;

  // Number of map shards must be a power of 2
  auto GetShard(const Key& key) const -> unsigned int {
    const auto num_shards = configs[MAP_SHARDS];
    assert((num_shards & (num_shards - 1)) == 0);
    const auto shard_bits = __builtin_ctz(num_shards);
    const auto hash = static_cast<uint64_t>(std::hash<Key>{}(key));
    const auto dist_hash = hash * FIBONACCI_MULTIPLIER;
    const auto top_bits = 64 - shard_bits;
    const auto shard_index = static_cast<unsigned int>(dist_hash >> top_bits);
    return shard_index;
  }
};
