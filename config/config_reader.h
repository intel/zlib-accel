// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <iostream>
#include <map>

// The responsibility of this class is to parse the
// Config file and store it in the std::map
// Defined getter function getValue() to get the
// data from the std::map.

class ConfigReader {
 private:
  std::map<std::string, std::string> config_settings_map;

 public:
  bool ParseFile(std::string fileName);
  bool GetValue(std::string tag, int& value, int max_value = 100,
                int min_value = 0);
  bool GetValue(std::string tag, std::string& value);

  std::string DumpValues();

 private:
  // Helper function to trim the tag and value.
  bool isValidFileNameOrPath(const std::string& input);
  std::string Trim(const std::string& str,
                   const std::string& whitespace = " \t");
  std::string Reduce(const std::string& str, const std::string& fill = " ",
                     const std::string& whitespace = " \t");
};
