// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include <fuzzer/FuzzedDataProvider.h>

#include <thread>

#include "../config/config.h"
#include "../tests/test_utils.h"

using namespace config;

int GenerateWindowBits(FuzzedDataProvider& fuzzed_data) {
  int format = fuzzed_data.ConsumeIntegralInRange<int>(0, 2);
  switch (format) {
    case 0:  // deflate raw
      return -15;
    case 1:  // zlib
      return 15;
    case 2:  // gzip
      return 31;
  }
  return -15;
}

// Verify that deflate/inflate always succeed assuming zlib fallback is enabled.
// The rest of the options are read from config file.
void CompressDecompress(const uint8_t* input_data, size_t input_data_length,
                        int* fuzz_ret) {
  FuzzedDataProvider fuzzed_data(input_data, input_data_length);
  *fuzz_ret = 0;

  int window_bits_compress = GenerateWindowBits(fuzzed_data);
  int flush_compress = Z_FINISH;
  int window_bits_uncompress = window_bits_compress;
  int flush_uncompress = Z_SYNC_FLUSH;

  std::string input_str = fuzzed_data.ConsumeRemainingBytesAsString();
  const char* input = input_str.c_str();
  size_t input_length = input_str.length();

  // This test assumes zlib fallback is available
  SetConfig(USE_ZLIB_COMPRESS, 1);
  SetConfig(USE_ZLIB_UNCOMPRESS, 1);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, window_bits_compress,
                         flush_compress, &output_upper_bound, &execution_path);

  if (ret != Z_STREAM_END) {
    *fuzz_ret = 1;
    return;
  }

  char* uncompressed = nullptr;
  size_t uncompressed_length;
  size_t input_consumed;
  execution_path = UNDEFINED;
  ret = ZlibUncompress(compressed.c_str(), compressed.length(), input_length,
                       &uncompressed, &uncompressed_length, &input_consumed,
                       window_bits_uncompress, flush_uncompress, 1,
                       &execution_path);

  // A Z_OK return owns the partial prefix it produced and an error owns
  // nothing, so release unconditionally here -- delete[] on the null an error
  // leaves is a no-op, and anything else is the buffer this call is abandoning.
  if (ret != Z_STREAM_END) {
    *fuzz_ret = 1;
    delete[] uncompressed;
    return;
  }

  if (memcmp(uncompressed, input, uncompressed_length) != 0) {
    *fuzz_ret = 1;
    delete[] uncompressed;
    return;
  }

  delete[] uncompressed;
}

int LaunchThread(const uint8_t* input, size_t input_length) {
  int ret;
  std::thread t(CompressDecompress, input, input_length, &ret);
  t.join();
  return ret;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* input,
                                      size_t input_length) {
  // Thread-local variables flag as memory leak if test run on main thread. Run
  // test on separate thread.
  int ret = LaunchThread(input, input_length);

  switch (ret) {
    case 1:
      __builtin_trap();
    case 0:
    case -1:
    default:
      return ret;
  }

  return 0;
}
