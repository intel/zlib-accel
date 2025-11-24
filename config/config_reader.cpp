// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "config_reader.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "../logging.h"

constexpr int CUSTOM_PATH_MAX = 4096;

bool ConfigReader::GetValue(const std::string& tag, uint32_t& value,
                            uint32_t max_value, uint32_t min_value) {
  auto it = config_settings_map.find(tag);
  if (it == config_settings_map.end()) {
    return false;
  }

  try {
    size_t pos = 0;
    unsigned long temp = std::stoul(it->second, &pos);

    if (temp > UINT32_MAX) {
      Log(LogLevel::LOG_ERROR, "ConfigReader::GetValue Line ", __LINE__,
          " value exceeds uint32_t range for tag ", tag.c_str(), "\n");
      value = 0;
      return false;
    }

    if (pos != it->second.length() || temp < min_value || temp > max_value) {
      Log(LogLevel::LOG_ERROR, "ConfigReader::GetValue Line ", __LINE__,
          " invalid input value for tag ", tag.c_str(), "\n");
      value = 0;
      return false;
    }

    value = static_cast<uint32_t>(temp);
    return true;

  } catch (const std::exception&) {
    Log(LogLevel::LOG_ERROR, "ConfigReader::GetValue Line ", __LINE__,
        " invalid input value for tag ", tag.c_str(), "\n");
    value = 0;
    return false;
  }
}

bool ConfigReader::GetValue(const std::string& tag, std::string& value) {
  auto it = config_settings_map.find(tag);
  if (it == config_settings_map.end()) {
    return false;
  }

  value = it->second;

  if (tag == "log_file" && !IsValidFileNameOrPath(value)) {
    Log(LogLevel::LOG_ERROR, "ConfigReader::GetValue Line ", __LINE__,
        " invalid log_file value ", value.c_str(), "\n");
    value.clear();
    return false;
  }

  return true;
}

bool ConfigReader::ParseFile(const std::string& file_name) {
  std::ifstream input_file(file_name);

  if (!input_file) {
    return false;
  }

  std::string line;
  while (std::getline(input_file, line)) {
    // Remove comment lines
    auto comment_pos = line.find('#');
    std::string config_data = line.substr(0, comment_pos);

    // Remove ^M from config_data
    config_data.erase(std::remove(config_data.begin(), config_data.end(), '\r'),
                      config_data.end());

    if (config_data.empty()) {
      continue;
    }

    // Find delimiter
    auto delimiter_pos = config_data.find('=');

    if (delimiter_pos == std::string::npos) {
      continue;
    }

    // Split into tag and value
    std::string tag = config_data.substr(0, delimiter_pos);
    std::string value = config_data.substr(delimiter_pos + 1);

    // Trim whitespace
    tag = Reduce(tag);
    value = Reduce(value);

    if (tag.empty() || value.empty()) {
      continue;
    }

    config_settings_map[tag] = std::move(value);
  }

  return true;
}

std::string ConfigReader::Trim(const std::string& str,
                               const std::string& whitespace) {
  auto str_begin = str.find_first_not_of(whitespace);
  if (str_begin == std::string::npos) {
    return {};
  }

  auto str_end = str.find_last_not_of(whitespace);
  auto str_range = str_end - str_begin + 1;

  return str.substr(str_begin, str_range);
}

std::string ConfigReader::Reduce(const std::string& str,
                                 const std::string& fill,
                                 const std::string& whitespace) {
  // Trim first
  std::string result = Trim(str, whitespace);

  // Replace whitespace sequences with fill
  auto begin_space = result.find_first_of(whitespace);
  while (begin_space != std::string::npos) {
    auto end_space = result.find_first_not_of(whitespace, begin_space);
    auto range = end_space - begin_space;

    result.replace(begin_space, range, fill);

    auto new_start = begin_space + fill.length();
    begin_space = result.find_first_of(whitespace, new_start);
  }

  return result;
}

std::string ConfigReader::DumpValues() {
  std::stringstream values;
  for (const auto& pair : config_settings_map) {
    values << pair.first << " = " << pair.second << '\n';
  }
  return values.str();
}

bool ConfigReader::IsValidFileNameOrPath(const std::string& input) {
  // Check for null character
  if (input.find('\0') != std::string::npos) {
    return false;
  }

  // Check for length constraints
  if (input.length() > CUSTOM_PATH_MAX) {
    return false;
  }

  // Regular expression to match valid file names and paths
  static const std::regex valid_pattern("^[a-zA-Z0-9._/-]+$");
  return std::regex_match(input, valid_pattern);
}
