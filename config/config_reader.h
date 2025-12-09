// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <map>
#include <string>

// The responsibility of this class is to parse the
// Config file and store it in the std::map
// Defined getter function getValue() to get the
// data from the std::map.

class ConfigReader {
 private:
  std::map<std::string, std::string> config_settings_map;

 public:
  bool ParseFile(const std::string& file_name);
  bool GetValue(const std::string& tag, uint32_t& value,
                uint32_t max_value = 100, uint32_t min_value = 0);
  bool GetValue(const std::string& tag, std::string& value);

  std::string DumpValues();

 private:
  // Helper function to trim the tag and value.
  bool IsValidFileNameOrPath(const std::string& input);
  std::string Trim(const std::string& str,
                   const std::string& whitespace = " \t");
  std::string Reduce(const std::string& str, const std::string& fill = " ",
                     const std::string& whitespace = " \t");
};
