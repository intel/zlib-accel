// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "config_reader.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

#include "../logging.h"

#define PATH_MAX 4096

using namespace std;

bool ConfigReader::GetValue(std::string tag, int& value, int max_value,
                            int min_value) {
  bool ret_val = false;
  map<string, string>::iterator it;
  it = config_settings_map.find(tag);
  if (it != config_settings_map.end()) {
    char* p;
    value = (int)strtol((it->second).c_str(), &p, 10);
    if ((*p == 0) && (min_value <= value && value <= max_value)) {
      ret_val = true;
    } else {
      Log(LogLevel::LOG_INFO,
          "ConfigReader::GetValue  Line %d invalid input value for tag %s\n",
          __LINE__, tag.c_str());
      value = 0;
      ret_val = false;
    }
  }
  return ret_val;
}

bool ConfigReader::GetValue(std::string tag, std::string& value) {
  bool ret_val = false;
  map<string, string>::iterator it;
  it = config_settings_map.find(tag);
  if (it != config_settings_map.end()) {
    value = it->second;
    if ((tag == "log_file") && !IsValidFileNameOrPath(value)) {
      Log(LogLevel::LOG_INFO,
          "ConfigReader::GetValue  Line %d invalid log_file value  %s\n",
          __LINE__, value.c_str());
      value = "";
      ret_val = false;
    } else {
      ret_val = true;
    }
  }
  return ret_val;
}

bool ConfigReader::ParseFile(string file_name) {
  ifstream input_file;
  input_file.open(file_name.c_str());
  string delimiter = "=";
  int initPos = 0;

  if (input_file.fail()) {
    return false;
  }

  string line;
  while (getline(input_file, line)) {
    // Remove comment Lines
    size_t found = line.find_first_of('#');
    string config_data = line.substr(0, found);

    // Remove ^M from config_data
    config_data.erase(std::remove(config_data.begin(), config_data.end(), '\r'),
                      config_data.end());

    if (config_data.empty()) continue;

    size_t length = config_data.find(delimiter);

    string tag, value;

    if (length != string::npos) {
      tag = config_data.substr(initPos, length);
      value = config_data.substr(length + 1);
    }

    // Trim white spaces
    tag = Reduce(tag);
    value = Reduce(value);

    if (tag.empty() || value.empty()) continue;

    // Check if any of the tags is repeated more than one times
    // it needs to pick the latest one instead of the old one.

    // Search, if the tag is already present or not
    // If it is already present, then delete an existing one

    std::map<std::string, std::string>::iterator itr =
        config_settings_map.find(tag);
    if (itr != config_settings_map.end()) {
      config_settings_map.erase(tag);
    }

    config_settings_map.insert(std::pair<string, string>(tag, value));
  }
  return true;
}

std::string ConfigReader::Trim(const std::string& str,
                               const std::string& whitespace) {
  size_t str_begin = str.find_first_not_of(whitespace);
  if (str_begin == std::string::npos) return "";

  size_t str_end = str.find_last_not_of(whitespace);
  size_t str_range = str_end - str_begin + 1;

  return str.substr(str_begin, str_range);
}

std::string ConfigReader::Reduce(const std::string& str,
                                 const std::string& fill,
                                 const std::string& whitespace) {
  // trim first
  string result = Trim(str, whitespace);

  // replace sub ranges
  size_t begin_space = result.find_first_of(whitespace);
  while (begin_space != std::string::npos) {
    size_t end_space = result.find_first_not_of(whitespace, begin_space);
    size_t range = end_space - begin_space;

    result.replace(begin_space, range, fill);

    size_t newStart = begin_space + fill.length();
    begin_space = result.find_first_of(whitespace, newStart);
  }

  return result;
}

std::string ConfigReader::DumpValues() {
  std::stringstream values;
  map<string, string>::iterator it;
  for (it = config_settings_map.begin(); it != config_settings_map.end();
       ++it) {
    values << it->first << " = " << it->second << endl;
  }
  return values.str();
}

bool ConfigReader::IsValidFileNameOrPath(const std::string& input) {
  // Check for null character
  if (input.find('\0') != std::string::npos) {
    return false;
  }
  // Check for length constraints
  if (input.length() > PATH_MAX) {
    return false;
  }
  // Regular expression to match valid file names and paths
  std::regex validPattern("^[a-zA-Z0-9._/-]+$");
  return std::regex_match(input, validPattern);
}
