// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#ifdef USE_TBB
// tbb::concurrent_hash_map implementation
#include <tbb/concurrent_hash_map.h>  // portable: old TBB + oneTBB
#else
// stdlib implementation
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#endif

#include "config/config.h"

template <typename Key, typename Value>
class ShardedMap {
 public:
  // Use GetConfig() rather than configs[] directly: configs has hidden
  // visibility and is not accessible from external translation units under LTO.
  //
  // For the three global registry instances, LTO causes the constructor to run
  // before init_zlib_accel loads the config file, so num_shards_ is
  // initialised with the default value. InitStreamRegistries() calls Init()
  // afterwards to re-allocate with the configured shard count.
  ShardedMap() : num_shards_(config::GetConfig(config::MAP_SHARDS)) {
    pd_map_arr_ = std::make_unique<PaddedMapType[]>(num_shards_);
  }

  void Init() {
    num_shards_ = config::GetConfig(config::MAP_SHARDS);
    pd_map_arr_ = std::make_unique<PaddedMapType[]>(num_shards_);
  }

  auto Get(const Key& key) -> decltype(std::declval<Value>().get()) {
    const auto shard = GetShard(key);
#ifdef USE_TBB
    typename MapType::const_accessor acc;
    if (!pd_map_arr_[shard].map.find(acc, key)) {
      return nullptr;
    }
    return acc->second.get();
#else
    std::shared_lock lock(pd_map_arr_[shard].mutex);
    auto it = pd_map_arr_[shard].map.find(key);
    if (it == pd_map_arr_[shard].map.end()) {
      return nullptr;
    }
    return it->second.get();
#endif
  }

  void Set(const Key& key, Value&& value) {
    const auto shard = GetShard(key);
#ifdef USE_TBB
    typename MapType::accessor acc;
    pd_map_arr_[shard].map.insert(acc, key);
    acc->second = std::move(value);
#else
    std::unique_lock lock(pd_map_arr_[shard].mutex);
    pd_map_arr_[shard].map[key] = std::move(value);
#endif
  }

  void Unset(const Key& key) {
    const auto shard = GetShard(key);
#ifndef USE_TBB
    std::unique_lock lock(pd_map_arr_[shard].mutex);
#endif
    pd_map_arr_[shard].map.erase(key);
  }

 private:
  // Number of bytes in a cache line
  static constexpr std::size_t CACHE_LINE_SIZE = 64;

  // Fibonacci hash constant: value is (2^64 / phi) to spread bits uniformly,
  // where phi is golden ratio and 64 is machine word length - constant is
  // multiplied by hash and then shifted right by top bits to get shard index
  static constexpr uint64_t FIBONACCI_MULTIPLIER = 11400714819323198485ull;

#ifdef USE_TBB
  struct HashCompare {
    static auto hash(const Key& key) -> std::size_t {
      return std::hash<Key>{}(key);
    }

    static auto equal(const Key& lhs, const Key& rhs) -> bool {
      return lhs == rhs;
    }
  };

  using MapType = tbb::concurrent_hash_map<Key, Value, HashCompare>;
#else
  using MapType = std::unordered_map<Key, Value>;
#endif

  struct alignas(CACHE_LINE_SIZE) PaddedMapType {
    MapType map;
#ifndef USE_TBB
    mutable std::shared_mutex mutex;
#endif
  };
  std::unique_ptr<PaddedMapType[]> pd_map_arr_;
  unsigned int num_shards_;

  // Number of map shards must be a power of 2
  auto GetShard(const Key& key) const -> unsigned int {
    const auto shard_bits = __builtin_ctz(num_shards_);
    const auto hash = static_cast<uint64_t>(std::hash<Key>{}(key));
    const auto dist_hash = hash * FIBONACCI_MULTIPLIER;
    const auto top_bits = 64 - shard_bits;
    const auto shard_index = static_cast<unsigned int>(dist_hash >> top_bits);
    return shard_index;
  }
};
