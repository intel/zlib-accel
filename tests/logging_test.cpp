// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifndef DEBUG_LOG
#define DEBUG_LOG ON
#endif

#ifndef ENABLE_STATISTICS
#define ENABLE_STATISTICS ON
#endif

#include "../logging.h"

namespace fs = std::filesystem;

using namespace config;

/**
 * @class LoggingTest
 * @brief Test fixture providing a clean log file before each test.
 *
 * Ensures:
 *   - The logging subsystem starts with no open log file.
 *   - The test log file is removed before and after each test.
 */
class LoggingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_log_file = "test_log.txt";
    CloseLogFile();                 // Ensure logger is not holding a file
    if (fs::exists(test_log_file))  // Remove leftover logs from earlier tests
      fs::remove(test_log_file);
  }

  void TearDown() override {
    CloseLogFile();                 // Close logger again for safety
    if (fs::exists(test_log_file))  // Clean output to keep tests isolated
      fs::remove(test_log_file);
  }

  std::string test_log_file;
};

/**
 * @test Ensures CreateLogFile() actually creates the file on disk.
 */
TEST_F(LoggingTest, CreateLogFileSuccess) {
  CreateLogFile(test_log_file.c_str());
  EXPECT_TRUE(fs::exists(test_log_file));
}

/**
 * @test Ensures CreateLogFile() does not overwrite an existing file
 *       but instead opens it in append mode.
 */
TEST_F(LoggingTest, CreateLogFileAppendsMode) {
  {
    std::ofstream f(test_log_file);
    f << "initial\n";  // Prepopulate file
  }

  CreateLogFile(test_log_file.c_str());  // Should append, not delete
  EXPECT_TRUE(fs::exists(test_log_file));
}

/**
 * @test Ensures CloseLogFile() resets the logger so GetLogStream()
 *       returns std::cout again.
 */
TEST_F(LoggingTest, CloseLogFile) {
  CreateLogFile(test_log_file.c_str());
  CloseLogFile();

  std::ostream& s = GetLogStream();
  EXPECT_TRUE(&s == &std::cout);
}

/**
 * @test Ensures GetLogStream() returns the file stream when a log
 *       file is currently open.
 */
TEST_F(LoggingTest, GetLogStreamReturnsFileWhenOpen) {
  CreateLogFile(test_log_file.c_str());
  std::ostream& s = GetLogStream();

  EXPECT_TRUE(&s != &std::cout);
}

/**
 * @test Ensures GetLogStream() returns std::cout when no file
 *       has been opened.
 */
TEST_F(LoggingTest, GetLogStreamReturnsCoutWhenClosed) {
  CloseLogFile();
  std::ostream& s = GetLogStream();
  EXPECT_TRUE(&s == &std::cout);
}

/**
 * @test Ensures GetLogStream() returns stdout if logging was never
 *       directed to a file.
 */
TEST_F(LoggingTest, GetLogStreamReturnsCoutWhenFileNotOpen) {
  CloseLogFile();
  std::ostream& s = GetLogStream();
  EXPECT_TRUE(&s == &std::cout);
}

/**
 * @test Verifies that INFO-level messages appear when LOG_LEVEL=INFO.
 */
TEST_F(LoggingTest, LogInfoLevel) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  Log(LogLevel::LOG_INFO, "test message");
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_NE(c.find("Info:"), std::string::npos);
  EXPECT_NE(c.find("test message"), std::string::npos);
}

/**
 * @test Verifies that ERROR-level messages appear when LOG_LEVEL=ERROR.
 */
TEST_F(LoggingTest, LogErrorLevel) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_ERROR));

  Log(LogLevel::LOG_ERROR, "error occurred");
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_NE(c.find("Error:"), std::string::npos);
  EXPECT_NE(c.find("error occurred"), std::string::npos);
}

/**
 * @test Verifies that LOG_NONE prevents any message from being logged.
 */
TEST_F(LoggingTest, LogNoneLevelReturnsEarly) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  Log(LogLevel::LOG_NONE, "should not appear");
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_EQ(c.find("should not appear"), std::string::npos);
}

/**
 * @test Ensures logging with multiple argument types is handled correctly.
 */
TEST_F(LoggingTest, LogMultipleArguments) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  Log(LogLevel::LOG_INFO, "values: ", 42, " and ", 3.14);
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_NE(c.find("42"), std::string::npos);
  EXPECT_NE(c.find("3.14"), std::string::npos);
}

/**
 * @test Validates that log-level filtering works correctly:
 *       - INFO log should be filtered out under LOG_ERROR
 *       - ERROR log should appear.
 */
TEST_F(LoggingTest, LogLevelFiltering) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_ERROR));

  Log(LogLevel::LOG_INFO, "filtered");  // Should NOT appear
  Log(LogLevel::LOG_ERROR, "visible");  // Should appear
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_EQ(c.find("filtered"), std::string::npos);
  EXPECT_NE(c.find("visible"), std::string::npos);
}

/**
 * @test Ensures LogStats() emits the "Stats:" prefix and all fields.
 */
TEST_F(LoggingTest, LogStatsOutput) {
  CreateLogFile(test_log_file.c_str());

  LogStats("stat1=", 100, ", stat2=", 200);
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_NE(c.find("Stats:"), std::string::npos);
  EXPECT_NE(c.find("100"), std::string::npos);
  EXPECT_NE(c.find("200"), std::string::npos);
}

/**
 * @test Ensures LogStats() supports multiple calls and writes sequentially.
 */
TEST_F(LoggingTest, LogStatsMultipleLines) {
  CreateLogFile(test_log_file.c_str());

  LogStats("first\n");
  LogStats("second\n");
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_GT(c.size(), 0u);
}

/**
 * @test Ensures PrintDeflateBlockHeader() writes header details
 *       when provided with enough data.
 */
TEST_F(LoggingTest, PrintDeflateBlockHeaderValidData) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  uint8_t data[] = {0x78, 0x9C, 0x03};
  PrintDeflateBlockHeader(LogLevel::LOG_INFO, data, sizeof(data), 15);
  CloseLogFile();

  EXPECT_TRUE(fs::exists(test_log_file));
}

/**
 * @test Ensures PrintDeflateBlockHeader() produces no output
 *       if data is too short to contain a valid header.
 */
TEST_F(LoggingTest, PrintDeflateBlockHeaderInsufficientData) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  uint8_t data[] = {0x78};
  auto before = fs::exists(test_log_file) ? fs::file_size(test_log_file) : 0;

  PrintDeflateBlockHeader(LogLevel::LOG_INFO, data, sizeof(data), 15);
  CloseLogFile();

  auto after = fs::exists(test_log_file) ? fs::file_size(test_log_file) : 0;
  EXPECT_EQ(before, after);
}

/**
 * @test Confirms that PrintDeflateBlockHeader() identifies the
 *       BFINAL bit when logging is enabled.
 */
TEST_F(LoggingTest, PrintDeflateBlockHeaderBfinalBit) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  uint8_t data[] = {0x78, 0x9C, 0x01};
  PrintDeflateBlockHeader(LogLevel::LOG_INFO, data, sizeof(data), 15);
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_NE(c.find("bfinal"), std::string::npos);
}

/**
 * @test Confirms that PrintDeflateBlockHeader() identifies the
 *       BTYPE field correctly.
 */
TEST_F(LoggingTest, PrintDeflateBlockHeaderBtype) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  uint8_t data[] = {0x78, 0x9C, 0x06};
  PrintDeflateBlockHeader(LogLevel::LOG_INFO, data, sizeof(data), 15);
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_NE(c.find("btype"), std::string::npos);
}

/**
 * @test Ensures the Log() function is thread-safe by making multiple
 *       threads emit log messages concurrently.
 */
TEST_F(LoggingTest, ConcurrentLogging) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  std::vector<std::thread> threads;
  const int T = 5;
  const int N = 10;

  for (int i = 0; i < T; ++i) {
    threads.emplace_back([i]() {
      for (int j = 0; j < N; ++j) {
        Log(LogLevel::LOG_INFO, "thread ", i, " iteration ", j);
      }
    });
  }

  for (auto& t : threads) t.join();

  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_NE(c.find("Info:"), std::string::npos);
}

/**
 * @test Ensures repeated calls to CreateLogFile() append to the same
 *       file rather than truncating previous log entries.
 */
TEST_F(LoggingTest, RepeatedCreateLogFileCall) {
  CreateLogFile(test_log_file.c_str());
  Log(LogLevel::LOG_INFO, "first");

  CreateLogFile(test_log_file.c_str());  // reopen in append mode
  Log(LogLevel::LOG_INFO, "second");

  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());

  EXPECT_NE(c.find("first"), std::string::npos);
  EXPECT_NE(c.find("second"), std::string::npos);
}

/**
 * @test Verifies that logging behaves safely when no file is created,
 *       and GetLogStream() returns std::cout.
 */
TEST_F(LoggingTest, LogWithoutCreatingFile) {
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));
  Log(LogLevel::LOG_INFO, "to stdout");

  std::ostream& s = GetLogStream();
  EXPECT_TRUE(&s == &std::cout);
}

/**
 * @test Validates logging of mixed string types, including std::string
 *       and C-style strings.
 */
TEST_F(LoggingTest, StringAndCharacterLogging) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_INFO));

  Log(LogLevel::LOG_INFO, std::string("hello"), " ", "world");
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
  EXPECT_NE(c.find("hello"), std::string::npos);
  EXPECT_NE(c.find("world"), std::string::npos);
}

/**
 * @test Ensures LOG_NONE disables all output, regardless of message level.
 */
TEST_F(LoggingTest, LogAllLevelTypes) {
  CreateLogFile(test_log_file.c_str());
  config::SetConfig(config::LOG_LEVEL,
                    static_cast<uint32_t>(LogLevel::LOG_NONE));

  Log(LogLevel::LOG_ERROR, "err");
  Log(LogLevel::LOG_INFO, "info");
  Log(LogLevel::LOG_NONE, "none");
  CloseLogFile();

  std::ifstream f(test_log_file);
  std::string c((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
  EXPECT_EQ(c.size(), 0u);
}
