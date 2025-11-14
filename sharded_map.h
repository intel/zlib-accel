// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

static constexpr int SHARDS = 64;

template <typename Key, typename Value>
class ShardedMap {
 public:
  auto Get(Key key) -> decltype(std::declval<Value>().get()) {
    unsigned int shard = GetShard(key);
    std::shared_lock<std::shared_mutex> lock(shard_mutexes[shard]);
    auto it = map[shard].find(key);
    if (it == map[shard].end()) {
      return nullptr;
    }
    return it->second.get();
  }

  void Set(Key key, Value&& value) {
    unsigned int shard = GetShard(key);
    std::unique_lock<std::shared_mutex> lock(shard_mutexes[shard]);
    map[shard][key] = std::move(value);
  }

  void Unset(Key key) {
    unsigned int shard = GetShard(key);
    std::unique_lock<std::shared_mutex> lock(shard_mutexes[shard]);
    auto it = map[shard].find(key);
    if (it != map[shard].end()) {
      map[shard].erase(it);
    }
  }

 private:
  unsigned int GetShard(Key key) { return std::hash<Key>{}(key) % SHARDS; }
  std::unordered_map<Key, Value> map[SHARDS];
  std::shared_mutex shard_mutexes[SHARDS];
  std::hash<std::string> hash;
};
