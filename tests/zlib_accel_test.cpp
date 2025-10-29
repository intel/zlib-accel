// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "../zlib_accel.h"

#include <gtest/gtest.h>
#include <stdio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>

#include "../config/config.h"
#include "../iaa.h"
#include "../qat.h"
#include "../sharded_map.h"
#include "../statistics.h"
#include "../utils.h"
#include "test_utils.h"

using namespace config;

enum BlockCompressibilityType {
  compressible_block,
  incompressible_block,
  zero_block
};

std::string GenerateRandomString(size_t length) {
  std::string random_string;
  for (unsigned int i = 0; i < length; i++) {
    char c = std::rand() % (std::numeric_limits<char>::max() -
                            std::numeric_limits<char>::min()) +
             std::numeric_limits<char>::min();
    random_string.push_back(c);
  }
  return random_string;
}

char* GenerateCompressibleBlock(size_t length, int ratio = 4) {
  char* buf = (char*)malloc(length);
  if (!buf) {
    return nullptr;
  }

  const unsigned int compressible_string_length = 1024;
  unsigned int random_string_length = compressible_string_length / ratio;
  const unsigned int long_range = 8192;
  std::string random_string_long_range =
      GenerateRandomString(random_string_length);
  std::string random_string;
  unsigned int pos = 0;
  while (pos < length) {
    if (pos % compressible_string_length == 0) {
      random_string = GenerateRandomString(random_string_length);
    }
    if ((pos % long_range) < random_string_length) {
      buf[pos] = random_string_long_range[pos % random_string_length];
    } else {
      buf[pos] = random_string[pos % random_string_length];
    }
    pos++;
  }

  return buf;
}

char* GenerateIncompressibleBlock(size_t length) {
  char* buf = (char*)malloc(length);
  if (!buf) {
    return nullptr;
  }
  std::string random_string = GenerateRandomString(length);
  for (unsigned int i = 0; i < length; i++) {
    buf[i] = random_string[i];
  }
  return buf;
}

char* GenerateZeroBlock(size_t length) {
  char* buf = (char*)calloc(length, sizeof(char));
  if (!buf) {
    return nullptr;
  }
  return buf;
}

char* GenerateBlock(size_t length, BlockCompressibilityType block_type) {
  switch (block_type) {
    case compressible_block:
      return GenerateCompressibleBlock(length);
    case incompressible_block:
      return GenerateIncompressibleBlock(length);
    default:
      return GenerateZeroBlock(length);
  }
}

void DestroyBlock(char* buf) { free(buf); }

int ZlibCompressUtility(const char* input, size_t input_length,
                        std::string* output, size_t* output_upper_bound) {
  Bytef* source = (Bytef*)input;
  long unsigned int sourceLen = static_cast<long unsigned int>(input_length);

  *output_upper_bound = compressBound(static_cast<unsigned long>(input_length));
  output->resize(*output_upper_bound);
  long unsigned int destLen =
      static_cast<long unsigned int>(*output_upper_bound);
  Bytef* dest = reinterpret_cast<Bytef*>(&(*output)[0]);

  int st = compress(dest, &destLen, source, sourceLen);
  if (st != Z_OK) {
    return st;
  }
  output->resize(destLen);
  return st;
}

int ZlibUncompressUtility(const char* input, size_t input_length,
                          size_t output_length, char** uncompressed,
                          size_t* uncompressed_length) {
  *uncompressed = new char[output_length];
  *uncompressed_length = 0;

  Bytef* source = (Bytef*)(input);
  long unsigned int sourceLen = static_cast<long unsigned int>(input_length);
  Bytef* dest = (Bytef*)(*uncompressed);
  long unsigned int destLen = static_cast<unsigned int>(output_length);

  int st = uncompress(dest, &destLen, source, sourceLen);
  if (st != Z_OK) {
    return st;
  }
  *uncompressed_length = destLen;
  return st;
}

int ZlibCompressUtility2(const char* input, size_t input_length,
                         std::string* output, size_t* output_upper_bound) {
  Bytef* source = (Bytef*)input;
  long unsigned int sourceLen = static_cast<long unsigned int>(input_length);

  *output_upper_bound = compressBound(static_cast<unsigned long>(input_length));
  output->resize(*output_upper_bound);
  long unsigned int destLen =
      static_cast<long unsigned int>(*output_upper_bound);
  Bytef* dest = reinterpret_cast<Bytef*>(&(*output)[0]);

  int st = compress2(dest, &destLen, source, sourceLen, Z_DEFAULT_COMPRESSION);
  if (st != Z_OK) {
    return st;
  }
  output->resize(destLen);
  return st;
}

int ZlibUncompressUtility2(const char* input, size_t input_length,
                           size_t output_length, char** uncompressed,
                           size_t* uncompressed_length) {
  *uncompressed = new char[output_length];
  *uncompressed_length = 0;

  Bytef* source = (Bytef*)(input);
  long unsigned int sourceLen = static_cast<long unsigned int>(input_length);
  Bytef* dest = (Bytef*)(*uncompressed);
  long unsigned int destLen = static_cast<unsigned int>(output_length);

  int st = uncompress2(dest, &destLen, source, &sourceLen);
  if (st != Z_OK) {
    return st;
  }
  *uncompressed_length = destLen;
  return st;
}

int ZlibCompressGzipFile(const char* input, size_t input_length) {
  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen("file.gz", "wb");
  int ret = gzwrite(fp, input, input_length);
  if (ret == 0) {
    ret = -1;
    gzclose(fp);
    return ret;
  }
  return gzclose(fp);
}

int ZlibUncompressGzipFile(size_t output_length, char** uncompressed,
                           size_t* uncompressed_length) {
  *uncompressed = new char[output_length];
  *uncompressed_length = 0;

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  int ret = gzread(fp, *uncompressed, output_length);

  if (ret == -1) {
    gzclose(fp);
    return ret;
  } else {
    *uncompressed_length = ret;
  }
  ret = gzclose(fp);
  remove(filename);
  return ret;
}

int ZlibUncompressGzipFileInChunks(size_t output_length, char** uncompressed,
                                   size_t* uncompressed_length,
                                   size_t chunk_size) {
  *uncompressed = new char[output_length];
  *uncompressed_length = 0;

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  size_t output_pos = 0;
  while (output_pos < output_length) {
    output_pos += gzread(fp, *uncompressed + output_pos, chunk_size);
  }
  *uncompressed_length = output_pos;
  int st = gzclose(fp);
  remove(filename);
  return st;
}

void SetCompressPath(ExecutionPath path, bool zlib_fallback,
                     bool iaa_prepend_empty_block,
                     bool qat_compression_allow_chunking) {
  switch (path) {
    case ZLIB:
      SetConfig(USE_IAA_COMPRESS, 0);
      SetConfig(USE_QAT_COMPRESS, 0);
      SetConfig(USE_ZLIB_COMPRESS, 1);
      break;
    case QAT:
      SetConfig(USE_IAA_COMPRESS, 0);
      SetConfig(USE_QAT_COMPRESS, 1);
      SetConfig(USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IAA:
      SetConfig(USE_IAA_COMPRESS, 1);
      SetConfig(USE_QAT_COMPRESS, 0);
      SetConfig(USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
      break;
    default:
      break;
  }
  SetConfig(IAA_PREPEND_EMPTY_BLOCK, iaa_prepend_empty_block);
  SetConfig(QAT_COMPRESSION_ALLOW_CHUNKING, qat_compression_allow_chunking);
}

void SetUncompressPath(ExecutionPath path, bool zlib_fallback,
                       bool iaa_prepend_empty_block) {
  switch (path) {
    case ZLIB:
      SetConfig(USE_IAA_UNCOMPRESS, 0);
      SetConfig(USE_QAT_UNCOMPRESS, 0);
      SetConfig(USE_ZLIB_UNCOMPRESS, 1);
      break;
    case QAT:
      SetConfig(USE_IAA_UNCOMPRESS, 0);
      SetConfig(USE_QAT_UNCOMPRESS, 1);
      SetConfig(USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IAA:
      SetConfig(USE_IAA_UNCOMPRESS, 1);
      SetConfig(USE_QAT_UNCOMPRESS, 0);
      SetConfig(USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
      break;
    default:
      break;
  }
  SetConfig(IAA_PREPEND_EMPTY_BLOCK, iaa_prepend_empty_block);
}

struct TestParam {
  TestParam(ExecutionPath _execution_path_compress,
            bool _zlib_fallback_compress,
            ExecutionPath _execution_path_uncompress,
            bool _zlib_fallback_uncompress, int _window_bits_compress,
            int _flush_compress, int _window_bits_uncompress,
            int _flush_uncompress, int _input_chunks_uncompress,
            size_t _block_size, BlockCompressibilityType _block_type,
            bool _iaa_prepend_empty_block, bool _qat_compression_allow_chunking)
      : execution_path_compress(_execution_path_compress),
        zlib_fallback_compress(_zlib_fallback_compress),
        execution_path_uncompress(_execution_path_uncompress),
        zlib_fallback_uncompress(_zlib_fallback_uncompress),
        window_bits_compress(_window_bits_compress),
        flush_compress(_flush_compress),
        window_bits_uncompress(_window_bits_uncompress),
        flush_uncompress(_flush_uncompress),
        input_chunks_uncompress(_input_chunks_uncompress),
        block_size(_block_size),
        block_type(_block_type),
        iaa_prepend_empty_block(_iaa_prepend_empty_block),
        qat_compression_allow_chunking(_qat_compression_allow_chunking) {}

  ExecutionPath execution_path_compress;
  bool zlib_fallback_compress;
  ExecutionPath execution_path_uncompress;
  bool zlib_fallback_uncompress;
  int window_bits_compress;
  int flush_compress;
  int window_bits_uncompress;
  int flush_uncompress;
  int input_chunks_uncompress;
  size_t block_size;
  BlockCompressibilityType block_type;
  bool iaa_prepend_empty_block;
  bool qat_compression_allow_chunking;

  std::string ExecutionPathToString(ExecutionPath path) {
    switch (path) {
      case UNDEFINED:
        return "undefined";
      case ZLIB:
        return "zlib";
      case QAT:
        return "QAT";
      case IAA:
        return "IAA";
    }
    return "";
  }

  std::string BlockCompressibilityTypeToString(
      BlockCompressibilityType block_type) {
    switch (block_type) {
      case compressible_block:
        return "compressible block";
      case incompressible_block:
        return "incompressible block";
      case zero_block:
        return "zero block";
    }
    return "";
  }

  std::string ToString() {
    std::stringstream param_str;
    param_str << "execution_path_compress: "
              << ExecutionPathToString(execution_path_compress) << std::endl;
    param_str << "zlib_fallback_compress: " << zlib_fallback_compress
              << std::endl;
    param_str << "execution_path_uncompress: "
              << ExecutionPathToString(execution_path_uncompress) << std::endl;
    param_str << "zlib_fallback_uncompress: " << zlib_fallback_uncompress
              << std::endl;
    param_str << "window_bits_compress: " << window_bits_compress << std::endl;
    param_str << "flush_compress: " << flush_compress << std::endl;
    param_str << "window_bits_uncompress: " << window_bits_uncompress
              << std::endl;
    param_str << "flush_uncompress: " << flush_uncompress << std::endl;
    param_str << "input_chunks_uncompress: " << input_chunks_uncompress
              << std::endl;
    param_str << "block_size: " << block_size << std::endl;
    param_str << "block_type: " << BlockCompressibilityTypeToString(block_type)
              << std::endl;
    param_str << "iaa_prepend_empty_block: " << iaa_prepend_empty_block
              << std::endl;
    param_str << "qat_compression_allow_chunking: "
              << qat_compression_allow_chunking << std::endl;
    return param_str.str();
  }
};

bool ZlibCompressExpectFallback(TestParam test_param, size_t input_length,
                                size_t output_upper_bound) {
  (void)test_param;
  (void)input_length;
  (void)output_upper_bound;

  bool fallback_expected = false;
#ifdef USE_QAT
  // if QAT selected, but options not supported
  if (test_param.execution_path_compress == QAT &&
      !SupportedOptionsQAT(test_param.window_bits_compress, input_length)) {
    fallback_expected = true;
  }
#endif
#ifdef USE_IAA
  // if IAA selected, but options not supported
  if (test_param.execution_path_compress == IAA &&
      !SupportedOptionsIAA(test_param.window_bits_compress, input_length,
                           output_upper_bound)) {
    fallback_expected = true;
  }
#endif
  return fallback_expected;
}

bool ZlibCompressExpectError(TestParam test_param, size_t input_length,
                             size_t output_upper_bound) {
  bool fallback_expected =
      ZlibCompressExpectFallback(test_param, input_length, output_upper_bound);
  return fallback_expected && !test_param.zlib_fallback_compress;
}

bool ZlibUncompressExpectFallback(TestParam test_param, size_t input_length,
                                  std::string& compressed,
                                  size_t compressed_length,
                                  int window_bits_uncompress,
                                  bool compress_fallback,
                                  bool* accelerator_tried = nullptr) {
  (void)test_param;
  (void)input_length;
  (void)compressed;
  (void)compressed_length;
  (void)window_bits_uncompress;
  (void)compress_fallback;

  bool fallback_expected = false;
  bool accelerator_tried_val = false;
#ifdef USE_QAT
  if (test_param.execution_path_uncompress == QAT) {
    if (!SupportedOptionsQAT(
            window_bits_uncompress,
            compressed_length / test_param.input_chunks_uncompress)) {
      fallback_expected = true;
    } else if (input_length > QAT_HW_BUFF_SZ &&
               (test_param.execution_path_compress != QAT ||
                !test_param.qat_compression_allow_chunking)) {
      // If it was not compressed by QAT or QAT chunking is not allowed, it is
      // not chunked
      fallback_expected = true;
      accelerator_tried_val = true;
    } else if (input_length > QAT_HW_BUFF_SZ &&
               test_param.execution_path_compress == QAT &&
               ((GetCompressedFormat(window_bits_uncompress) ==
                     CompressedFormat::ZLIB &&
                 test_param.block_type == incompressible_block) ||
                GetCompressedFormat(window_bits_uncompress) ==
                    CompressedFormat::DEFLATE_RAW ||
                !test_param.qat_compression_allow_chunking)) {
      // If data was compressed with QAT, it was chunked during compression
      // (if chunking is allowed)
      // - gzip format: QAT decompression always possible (stream boundaries
      // detected before decompression)
      // - zlib format: QAT decompression possible if compressed
      // data fits in HW buffer size (it does not happen with incompressible
      // data).
      // - deflate raw: chunking during compression doesn't close the stream.
      // Decompression not possible.
      fallback_expected = true;
      accelerator_tried_val = true;
    } else if (test_param.input_chunks_uncompress > 1) {
      // Multi-chunk tests that were not skipped are expected to cause error
      fallback_expected = true;
      accelerator_tried_val = true;
    }
  }
#endif
#ifdef USE_IAA
  if (test_param.execution_path_uncompress == IAA) {
    if (!SupportedOptionsIAA(
            window_bits_uncompress,
            compressed_length / test_param.input_chunks_uncompress,
            input_length)) {
      fallback_expected = true;
    } else if (!IsIAADecompressible(
                   (uint8_t*)compressed.c_str(),
                   compressed_length / test_param.input_chunks_uncompress,
                   window_bits_uncompress)) {
      // IsIAADecompressible reports if block is decompressible by IAA
      // In some cases (when not looking for IAA marker) if may incorrectly
      // report block as IAA-decompressible.
      fallback_expected = true;
    } else if (test_param.execution_path_compress != IAA &&
               test_param.block_size > (4 << 10) &&
               test_param.block_type == compressible_block) {
      // If we cannot rely on marker, check if block was compressed by IAA, or
      // it is less than 4kB if compressible. Incompressible or zero blocks
      // don't need long-range references and can still be decompressed even if
      // larger than 4kB.
      fallback_expected = true;
      accelerator_tried_val = true;
    } else if (test_param.execution_path_compress == IAA && compress_fallback &&
               test_param.block_type == compressible_block) {
      // If IAA compression falls back to zlib (e.g., for 2MB blocks)
      // Incompressible or zero blocks don't need long-range references and can
      // still be decompressed
      fallback_expected = true;
      accelerator_tried_val = true;
    } else if (test_param.input_chunks_uncompress > 1) {
      // IAA with QPL_FLAG_LAST gets QPL_STS_BAD_EOF_ERR if a stream is not
      // decompressed in one call
      fallback_expected = true;
      accelerator_tried_val = true;
    }
  }
#endif
  if (accelerator_tried != nullptr) {
    *accelerator_tried = accelerator_tried_val;
  }
  return fallback_expected;
}

bool ZlibUncompressExpectError(TestParam test_param, size_t input_length,
                               std::string& compressed,
                               size_t compressed_length,
                               int window_bits_uncompress,
                               bool compress_fallback = false) {
  bool fallback_expected = ZlibUncompressExpectFallback(
      test_param, input_length, compressed, compressed_length,
      window_bits_uncompress, compress_fallback);
  return fallback_expected && !test_param.zlib_fallback_uncompress;
}

void VerifyStatIncremented(Statistic stat) {
  if (AreStatsEnabled()) {
    ASSERT_EQ(GetStat(stat), 1) << "Statistic id: " << stat;
  }
}

void VerifyStatIncrementedUpTo(Statistic stat, int up_to) {
  if (AreStatsEnabled()) {
    ASSERT_LE(GetStat(stat), up_to) << "Statistic id: " << stat;
  }
}

void RunDummyQATJob() {
  size_t input_length = 4096;
  char* input = GenerateBlock(input_length, compressible_block);
  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  ZlibCompress(input, input_length, &compressed, 15, 4, &output_upper_bound,
               &execution_path);
  char* uncompressed = nullptr;
  size_t uncompressed_length;
  size_t input_consumed;
  ZlibUncompress(compressed.c_str(), compressed.length(), input_length,
                 &uncompressed, &uncompressed_length, &input_consumed, 15, 1, 1,
                 &execution_path);
  delete[] uncompressed;
  DestroyBlock(input);
}

class ZlibTest
    : public testing::TestWithParam<
          std::tuple<ExecutionPath, bool, ExecutionPath, bool, int, int, int,
                     int, int, size_t, BlockCompressibilityType, bool, bool>> {
};

TEST_P(ZlibTest, CompressDecompress) {
  TestParam test_param(
      std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
      std::get<3>(GetParam()), std::get<4>(GetParam()), std::get<5>(GetParam()),
      std::get<6>(GetParam()), std::get<7>(GetParam()), std::get<8>(GetParam()),
      std::get<9>(GetParam()), std::get<10>(GetParam()),
      std::get<11>(GetParam()), std::get<12>(GetParam()));
  Log(test_param.ToString());

  // QAT does not support stateful decompression (decompression must be done in
  // one call)
  // We need to skip these tests rather then testing for errors, because
  // decompression may succeed in some cases if QAT compression chunk < test
  // chunk.
  if (test_param.execution_path_compress == QAT &&
      test_param.execution_path_uncompress != ZLIB &&
      test_param.input_chunks_uncompress > 1) {
    GTEST_SKIP();
  }

  if (test_param.execution_path_compress == IAA &&
      test_param.iaa_prepend_empty_block == 1 &&
      test_param.block_type == incompressible_block) {
    std::cout << "A prepended empty block may not fit in output buffer for "
                 "incompressible blocks\n";
    GTEST_SKIP();
  }

  // Capture statistics at beginning of run
  ResetStats();

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress,
                  test_param.iaa_prepend_empty_block,
                  test_param.qat_compression_allow_chunking);

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(
      input, input_length, &compressed, test_param.window_bits_compress,
      test_param.flush_compress, &output_upper_bound, &execution_path);
  VerifyStatIncremented(Statistic::DEFLATE_COUNT);

  bool compress_fallback_expected =
      ZlibCompressExpectFallback(test_param, input_length, output_upper_bound);
  if (compress_fallback_expected && !test_param.zlib_fallback_compress) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    VerifyStatIncremented(Statistic::DEFLATE_ERROR_COUNT);
    DestroyBlock(input);
    return;
  } else {
    ASSERT_EQ(ret, Z_STREAM_END);
    if (compress_fallback_expected) {
      ASSERT_EQ(execution_path, ZLIB);
      VerifyStatIncremented(Statistic::DEFLATE_ZLIB_COUNT);
    } else {
      ASSERT_EQ(execution_path, test_param.execution_path_compress);
      if (test_param.execution_path_compress == QAT) {
        VerifyStatIncremented(Statistic::DEFLATE_QAT_COUNT);
      } else if (test_param.execution_path_compress == IAA) {
        VerifyStatIncremented(Statistic::DEFLATE_IAA_COUNT);
      } else if (test_param.execution_path_compress == ZLIB) {
        VerifyStatIncremented(Statistic::DEFLATE_ZLIB_COUNT);
      }
    }
  }

  SetUncompressPath(test_param.execution_path_uncompress,
                    test_param.zlib_fallback_uncompress,
                    test_param.iaa_prepend_empty_block);

  char* uncompressed;
  size_t uncompressed_length;
  size_t input_consumed;
  execution_path = UNDEFINED;
  int window_bits_uncompress = test_param.window_bits_compress;
  if (test_param.window_bits_uncompress != 0) {
    window_bits_uncompress = test_param.window_bits_uncompress;
  }
  ret = ZlibUncompress(compressed.c_str(), compressed.length(), input_length,
                       &uncompressed, &uncompressed_length, &input_consumed,
                       window_bits_uncompress, test_param.flush_uncompress,
                       test_param.input_chunks_uncompress, &execution_path);
  VerifyStatIncrementedUpTo(Statistic::INFLATE_COUNT,
                            test_param.input_chunks_uncompress);

  bool error_expected = false;
  bool accelerator_tried = false;
  bool uncompress_fallback_expected = ZlibUncompressExpectFallback(
      test_param, input_length, compressed, compressed.length(),
      window_bits_uncompress, compress_fallback_expected, &accelerator_tried);
  if (uncompress_fallback_expected && !test_param.zlib_fallback_uncompress) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    VerifyStatIncremented(Statistic::INFLATE_ERROR_COUNT);
    if (accelerator_tried) {
      if (test_param.execution_path_uncompress == QAT) {
        VerifyStatIncremented(Statistic::INFLATE_QAT_ERROR_COUNT);
      } else if (test_param.execution_path_uncompress == IAA) {
        VerifyStatIncremented(Statistic::INFLATE_IAA_ERROR_COUNT);
      }
    }
    error_expected = true;
  } else {
    ASSERT_EQ(ret, Z_STREAM_END);
    if (uncompress_fallback_expected) {
      ASSERT_EQ(execution_path, ZLIB);
      VerifyStatIncrementedUpTo(Statistic::INFLATE_ZLIB_COUNT,
                                test_param.input_chunks_uncompress);
    } else {
      ASSERT_EQ(execution_path, test_param.execution_path_uncompress);
      if (test_param.execution_path_uncompress == QAT) {
        VerifyStatIncremented(Statistic::INFLATE_QAT_COUNT);
      } else if (test_param.execution_path_uncompress == IAA) {
        VerifyStatIncremented(Statistic::INFLATE_IAA_COUNT);
      } else if (test_param.execution_path_uncompress == ZLIB) {
        VerifyStatIncrementedUpTo(Statistic::INFLATE_ZLIB_COUNT,
                                  test_param.input_chunks_uncompress);
      }
    }
  }

  if (!error_expected) {
#ifdef USE_QAT
    if (test_param.execution_path_compress == QAT &&
        input_length > QAT_HW_BUFF_SZ &&
        test_param.qat_compression_allow_chunking &&
        GetCompressedFormat(window_bits_uncompress) !=
            CompressedFormat::DEFLATE_RAW) {
      // For data compressed by qzCompress, data is
      // made of multiple streams of hardware buffer size
      // (if chunking is allowed)
      ASSERT_TRUE(uncompressed_length <= QAT_HW_BUFF_SZ);
      ASSERT_TRUE(memcmp(uncompressed, input, uncompressed_length) == 0);
    } else {
      ASSERT_EQ(uncompressed_length, input_length);
      ASSERT_TRUE(memcmp(uncompressed, input, input_length) == 0);
    }
#else
    ASSERT_EQ(uncompressed_length, input_length);
    ASSERT_TRUE(memcmp(uncompressed, input, input_length) == 0);
#endif
  }

  // In case of QAT stateless overflow errors with zlib format, in some cases
  // QAT state is not properly reset. This causes subsequent tests to fail.
  // Tests pass if run individually. Running a dummy QAT compress/decompress job
  // mitigates the issue. For zlib-accel uses outside tests, the impact is
  // minimal (a few more jobs may fall back to zlib) and mitigation is not
  // necessary.
  // TODO investigate root cause and remove this mitigation.
  if (GetCompressedFormat(window_bits_uncompress) == CompressedFormat::ZLIB) {
    RunDummyQATJob();
  }

  delete[] uncompressed;
  DestroyBlock(input);
}

INSTANTIATE_TEST_SUITE_P(
    CompressDecompress, ZlibTest,
    testing::Combine(
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true),
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true), testing::Values(-15, 15, 31),
        // testing::Values(Z_NO_FLUSH, Z_PARTIAL_FLUSH,
        // Z_SYNC_FLUSH, Z_FULL_FLUSH, Z_FINISH, Z_BLOCK, Z_TREES),
        testing::Values(Z_FINISH), testing::Values(0),
        testing::Values(Z_PARTIAL_FLUSH, Z_SYNC_FLUSH), testing::Values(1, 2),
        testing::Values(1024, 4096, 16384, 262144, 2097152),
        testing::Values(compressible_block, incompressible_block, zero_block),
        testing::Values(false, true),   /* iaa_prepend_empty_block */
        testing::Values(false, true))); /* qat_compression_allow_chunking */

class ZlibUtilityTest : public ZlibTest {};

TEST_P(ZlibUtilityTest, CompressDecompressUtility) {
  TestParam test_param(
      std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
      std::get<3>(GetParam()), std::get<4>(GetParam()), std::get<5>(GetParam()),
      std::get<6>(GetParam()), std::get<7>(GetParam()), std::get<8>(GetParam()),
      std::get<9>(GetParam()), std::get<10>(GetParam()),
      std::get<11>(GetParam()), std::get<12>(GetParam()));
  Log(test_param.ToString());

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress,
                  test_param.iaa_prepend_empty_block,
                  test_param.qat_compression_allow_chunking);

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  int ret = ZlibCompressUtility(input, input_length, &compressed,
                                &output_upper_bound);

  bool error_expected =
      ZlibCompressExpectError(test_param, input_length, output_upper_bound);
  if (error_expected) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    DestroyBlock(input);
    return;
  } else {
    ASSERT_EQ(ret, Z_OK);
  }

  SetUncompressPath(test_param.execution_path_uncompress,
                    test_param.zlib_fallback_uncompress,
                    test_param.iaa_prepend_empty_block);

  char* uncompressed;
  size_t uncompressed_length;
  ret =
      ZlibUncompressUtility(compressed.c_str(), compressed.length(),
                            input_length, &uncompressed, &uncompressed_length);

  error_expected = ZlibUncompressExpectError(test_param, input_length,
                                             compressed, compressed.length(),
                                             test_param.window_bits_compress);
  if (error_expected) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
  } else {
    ASSERT_EQ(ret, Z_OK);
  }

  if (!error_expected) {
    ASSERT_EQ(uncompressed_length, input_length);
    ASSERT_TRUE(memcmp(uncompressed, input, input_length) == 0);
  }

  delete[] uncompressed;
  DestroyBlock(input);
}

INSTANTIATE_TEST_SUITE_P(
    CompressDecompress, ZlibUtilityTest,
    testing::Combine(
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true),
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true), testing::Values(15),
        testing::Values(Z_FINISH), testing::Values(0),
        testing::Values(Z_SYNC_FLUSH), testing::Values(1),
        testing::Values(1024, 4096, 16384, 262144),
        testing::Values(compressible_block, incompressible_block, zero_block),
        testing::Values(false),  /* iaa_prepend_empty_block */
        testing::Values(true))); /* qat_compression_allow_chunking */

class ZlibUtility2Test : public ZlibTest {};

TEST_P(ZlibUtility2Test, CompressDecompressUtility2) {
  TestParam test_param(
      std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
      std::get<3>(GetParam()), std::get<4>(GetParam()), std::get<5>(GetParam()),
      std::get<6>(GetParam()), std::get<7>(GetParam()), std::get<8>(GetParam()),
      std::get<9>(GetParam()), std::get<10>(GetParam()),
      std::get<11>(GetParam()), std::get<12>(GetParam()));
  Log(test_param.ToString());

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress,
                  test_param.iaa_prepend_empty_block,
                  test_param.qat_compression_allow_chunking);

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  int ret = ZlibCompressUtility2(input, input_length, &compressed,
                                 &output_upper_bound);

  bool error_expected =
      ZlibCompressExpectError(test_param, input_length, output_upper_bound);
  if (error_expected) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    DestroyBlock(input);
    return;
  } else {
    ASSERT_EQ(ret, Z_OK);
  }

  SetUncompressPath(test_param.execution_path_uncompress,
                    test_param.zlib_fallback_uncompress,
                    test_param.iaa_prepend_empty_block);

  char* uncompressed;
  size_t uncompressed_length;
  ret =
      ZlibUncompressUtility2(compressed.c_str(), compressed.length(),
                             input_length, &uncompressed, &uncompressed_length);

  error_expected = ZlibUncompressExpectError(test_param, input_length,
                                             compressed, compressed.length(),
                                             test_param.window_bits_compress);
  if (error_expected) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
  } else {
    ASSERT_EQ(ret, Z_OK);
  }

  if (!error_expected) {
    ASSERT_EQ(uncompressed_length, input_length);
    ASSERT_TRUE(memcmp(uncompressed, input, input_length) == 0);
  }

  delete[] uncompressed;
  DestroyBlock(input);
}

INSTANTIATE_TEST_SUITE_P(
    CompressDecompress, ZlibUtility2Test,
    testing::Combine(
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true),
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true), testing::Values(15),
        testing::Values(Z_FINISH), testing::Values(0),
        testing::Values(Z_SYNC_FLUSH), testing::Values(1),
        testing::Values(1024, 4096, 16384, 262144),
        testing::Values(compressible_block, incompressible_block, zero_block),
        testing::Values(false),  /* iaa_prepend_empty_block */
        testing::Values(true))); /* qat_compression_allow_chunking */

class ZlibPartialAndMultiStreamTest : public ZlibTest {};

TEST_P(ZlibPartialAndMultiStreamTest, CompressDecompressPartialStream) {
  TestParam test_param(
      std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
      std::get<3>(GetParam()), std::get<4>(GetParam()), std::get<5>(GetParam()),
      std::get<6>(GetParam()), std::get<7>(GetParam()), std::get<8>(GetParam()),
      std::get<9>(GetParam()), std::get<10>(GetParam()),
      std::get<11>(GetParam()), std::get<12>(GetParam()));
  Log(test_param.ToString());

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress,
                  test_param.iaa_prepend_empty_block,
                  test_param.qat_compression_allow_chunking);

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(
      input, input_length, &compressed, test_param.window_bits_compress,
      test_param.flush_compress, &output_upper_bound, &execution_path);

  bool error_expected =
      ZlibCompressExpectError(test_param, input_length, output_upper_bound);
  if (error_expected) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    DestroyBlock(input);
    return;
  } else {
    ASSERT_EQ(ret, Z_STREAM_END);
  }

  SetUncompressPath(test_param.execution_path_uncompress,
                    test_param.zlib_fallback_uncompress,
                    test_param.iaa_prepend_empty_block);

  // Decompress half of the first stream
  char* uncompressed;
  size_t uncompressed_length;
  size_t input_consumed;
  execution_path = UNDEFINED;
  int window_bits_uncompress = test_param.window_bits_compress;
  size_t compressed_length = compressed.length() / 2;
  ret = ZlibUncompress(compressed.c_str(), compressed_length, input_length,
                       &uncompressed, &uncompressed_length, &input_consumed,
                       window_bits_uncompress, test_param.flush_uncompress,
                       test_param.input_chunks_uncompress, &execution_path);

  // Only zlib decompression won't return an error
  if (test_param.execution_path_uncompress == ZLIB ||
      test_param.zlib_fallback_uncompress) {
    ASSERT_EQ(ret, Z_OK);
    ASSERT_TRUE(uncompressed_length < input_length);
    ASSERT_TRUE(memcmp(uncompressed, input, uncompressed_length) == 0);
  } else {
    ASSERT_EQ(ret, Z_DATA_ERROR);
  }

  delete[] uncompressed;
  DestroyBlock(input);
}

TEST_P(ZlibPartialAndMultiStreamTest, CompressDecompressMultiStream) {
  TestParam test_param(
      std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
      std::get<3>(GetParam()), std::get<4>(GetParam()), std::get<5>(GetParam()),
      std::get<6>(GetParam()), std::get<7>(GetParam()), std::get<8>(GetParam()),
      std::get<9>(GetParam()), std::get<10>(GetParam()),
      std::get<11>(GetParam()), std::get<12>(GetParam()));
  Log(test_param.ToString());

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress,
                  test_param.iaa_prepend_empty_block,
                  test_param.qat_compression_allow_chunking);

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  // Compress data in 2 streams
  std::string compressed1;
  size_t input_length1 = input_length / 2;
  size_t output_upper_bound1;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(
      input, input_length1, &compressed1, test_param.window_bits_compress,
      test_param.flush_compress, &output_upper_bound1, &execution_path);

  bool error_expected =
      ZlibCompressExpectError(test_param, input_length1, output_upper_bound1);
  if (error_expected) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    DestroyBlock(input);
    return;
  } else {
    ASSERT_EQ(ret, Z_STREAM_END);
  }

  std::string compressed2;
  size_t input_length2 = input_length - input_length / 2;
  size_t output_upper_bound2;
  execution_path = UNDEFINED;
  ret = ZlibCompress(input + input_length1, input_length2, &compressed2,
                     test_param.window_bits_compress, test_param.flush_compress,
                     &output_upper_bound2, &execution_path);

  error_expected =
      ZlibCompressExpectError(test_param, input_length2, output_upper_bound2);
  if (error_expected) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    DestroyBlock(input);
    return;
  } else {
    ASSERT_EQ(ret, Z_STREAM_END);
  }

  std::string compressed = compressed1 + compressed2;

  SetUncompressPath(test_param.execution_path_uncompress,
                    test_param.zlib_fallback_uncompress,
                    test_param.iaa_prepend_empty_block);

  // Decompress all the first stream and half of the second
  char* uncompressed;
  size_t uncompressed_length;
  size_t input_consumed;
  execution_path = UNDEFINED;
  int window_bits_uncompress = test_param.window_bits_compress;
  size_t compressed_length = compressed1.length() + compressed2.length() / 2;
  ret = ZlibUncompress(compressed.c_str(), compressed_length, input_length,
                       &uncompressed, &uncompressed_length, &input_consumed,
                       window_bits_uncompress, test_param.flush_uncompress,
                       test_param.input_chunks_uncompress, &execution_path);

  error_expected =
      ZlibUncompressExpectError(test_param, input_length, compressed,
                                compressed_length, window_bits_uncompress);
  if (error_expected) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
  } else {
    ASSERT_EQ(ret, Z_STREAM_END);
  }

  if (!error_expected) {
    // Decompression ends at the end of the first stream
    ASSERT_EQ(ret, Z_STREAM_END);
    ASSERT_EQ(uncompressed_length, input_length1);
    ASSERT_TRUE(memcmp(uncompressed, input, uncompressed_length) == 0);

    // IAA does not handle concatenated streams
    if (test_param.execution_path_uncompress != IAA) {
      ASSERT_EQ(input_consumed, compressed1.length());
    }
  }

  delete[] uncompressed;
  DestroyBlock(input);
}

INSTANTIATE_TEST_SUITE_P(
    CompressDecompress, ZlibPartialAndMultiStreamTest,
    testing::Combine(
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true),
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true), testing::Values(-15, 15, 31),
        testing::Values(Z_FINISH), testing::Values(0),
        testing::Values(Z_SYNC_FLUSH), testing::Values(1),
        // Testing 32k instead of 16k blocks, to make IAA success/failure
        // predictable. With 16k divided into two 8k streams, sometimes IAA is
        // able to decompress if no references happened to be farther than 4kB.
        testing::Values(1024, 32768, 262144),
        testing::Values(compressible_block, incompressible_block),
        testing::Values(false),  /* iaa_prepend_empty_block */
        testing::Values(true))); /* qat_compression_allow_chunking */

class ZlibGzipFileTest : public ZlibTest {};

TEST_P(ZlibGzipFileTest, CompressDecompressGzipFile) {
  TestParam test_param(
      std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
      std::get<3>(GetParam()), std::get<4>(GetParam()), std::get<5>(GetParam()),
      std::get<6>(GetParam()), std::get<7>(GetParam()), std::get<8>(GetParam()),
      std::get<9>(GetParam()), std::get<10>(GetParam()),
      std::get<11>(GetParam()), std::get<12>(GetParam()));
  Log(test_param.ToString());

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress,
                  test_param.iaa_prepend_empty_block,
                  test_param.qat_compression_allow_chunking);

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  int ret = ZlibCompressGzipFile(input, input_length);
  ASSERT_EQ(ret, Z_OK);

  SetUncompressPath(test_param.execution_path_uncompress,
                    test_param.zlib_fallback_uncompress,
                    test_param.iaa_prepend_empty_block);

  char* uncompressed;
  size_t uncompressed_length;
  if (test_param.input_chunks_uncompress == 1) {
    ret = ZlibUncompressGzipFile(input_length, &uncompressed,
                                 &uncompressed_length);
  } else {
    ret = ZlibUncompressGzipFileInChunks(
        input_length, &uncompressed, &uncompressed_length,
        input_length / test_param.input_chunks_uncompress);
  }
  ASSERT_EQ(ret, Z_OK);
  ASSERT_EQ(uncompressed_length, input_length);
  ASSERT_TRUE(memcmp(uncompressed, input, input_length) == 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

INSTANTIATE_TEST_SUITE_P(
    CompressDecompress, ZlibGzipFileTest,
    testing::Combine(
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true),
        testing::Values(ZLIB
#ifdef USE_QAT
                        ,
                        QAT
#endif
#ifdef USE_IAA
                        ,
                        IAA
#endif
                        ),
        testing::Values(false, true), testing::Values(31),
        testing::Values(Z_FINISH), testing::Values(0),
        testing::Values(Z_SYNC_FLUSH), testing::Values(1, 10),
        testing::Values(1024, 16384, 262144, 2097152),
        testing::Values(compressible_block, incompressible_block),
        testing::Values(false),  /* iaa_prepend_empty_block */
        testing::Values(true))); /* qat_compression_allow_chunking */

class ConfigLoaderTest : public ::testing::Test {};

void CreateAndWriteTempConfigFile(const char* file_path) {
  std::ofstream temp_file(file_path);
  temp_file << "use_qat_compress=5000\n";
  temp_file << "use_qat_uncompress=aaaa\n";
  temp_file << "use_iaa_compress=!0\n";
  temp_file << "use_iaa_compress=!0\n";
  temp_file << "use_zlib_compress=!0222\n";
  temp_file << "use_zlib_uncompress=AB23\n";
  temp_file << "log_level=10\n";
  temp_file << "log_stats_samples=4294967296\n";
  temp_file.close();
}

TEST_F(ConfigLoaderTest, LoadInvalidConfig) {
  std::string file_content;
  uint32_t DEFAULT_QAT_COMPRESS = GetConfig(USE_QAT_COMPRESS);
  uint32_t DEFAULT_QAT_UNCOMPRESS = GetConfig(USE_QAT_UNCOMPRESS);
  uint32_t DEFAULT_IAA_COMPRESS = GetConfig(USE_IAA_COMPRESS);
  uint32_t DEFAULT_IAA_UNCOMPRESS = GetConfig(USE_IAA_UNCOMPRESS);
  uint32_t DEFAULT_ZLIB_COMPRESS = GetConfig(USE_ZLIB_COMPRESS);
  uint32_t DEFAULT_ZLIB_UNCOMPRESS = GetConfig(USE_ZLIB_UNCOMPRESS);
  uint32_t DEFAULT_LOG_LEVEL = GetConfig(LOG_LEVEL);
  uint32_t DEFAULT_LOG_STATS_SAMPLES = GetConfig(LOG_STATS_SAMPLES);

  CreateAndWriteTempConfigFile("/tmp/invalid_config");
  EXPECT_TRUE(LoadConfigFile(file_content, "/tmp/invalid_config"));
  EXPECT_EQ(GetConfig(USE_QAT_COMPRESS), DEFAULT_QAT_COMPRESS);
  EXPECT_EQ(GetConfig(USE_QAT_UNCOMPRESS), DEFAULT_QAT_UNCOMPRESS);
  EXPECT_EQ(GetConfig(USE_IAA_COMPRESS), DEFAULT_IAA_COMPRESS);
  EXPECT_EQ(GetConfig(USE_IAA_UNCOMPRESS), DEFAULT_IAA_UNCOMPRESS);
  EXPECT_EQ(GetConfig(USE_ZLIB_COMPRESS), DEFAULT_ZLIB_COMPRESS);
  EXPECT_EQ(GetConfig(USE_ZLIB_UNCOMPRESS), DEFAULT_ZLIB_UNCOMPRESS);
  EXPECT_EQ(GetConfig(LOG_LEVEL), DEFAULT_LOG_LEVEL);
  EXPECT_EQ(GetConfig(LOG_STATS_SAMPLES), DEFAULT_LOG_STATS_SAMPLES);
  std::remove("/tmp/invalid_config");
  // Restore config from official config file
  LoadConfigFile(file_content);
}

TEST_F(ConfigLoaderTest, LoadValidConfig) {
  std::string file_content;
  EXPECT_TRUE(LoadConfigFile(file_content, "../../config/default_config"));
  EXPECT_EQ(GetConfig(USE_QAT_COMPRESS), 1);
  EXPECT_EQ(GetConfig(USE_QAT_UNCOMPRESS), 1);
  EXPECT_EQ(GetConfig(USE_IAA_COMPRESS), 0);
  EXPECT_EQ(GetConfig(USE_IAA_UNCOMPRESS), 0);
  EXPECT_EQ(GetConfig(USE_ZLIB_COMPRESS), 1);
  EXPECT_EQ(GetConfig(USE_ZLIB_UNCOMPRESS), 1);
  EXPECT_EQ(GetConfig(LOG_LEVEL), 2);
  LoadConfigFile(file_content);
}

TEST_F(ConfigLoaderTest, SymbolicLinkTest) {
  std::string file_content;
  std::filesystem::path target_path = "/tmp/target_file_path";
  std::filesystem::path symlink_path = "symlink_to_target";
  // create a real/target file
  std::ofstream target_file(target_path);
  target_file.close();
  // create a symlink for the target file
  std::filesystem::create_symlink(target_path, symlink_path);
  EXPECT_FALSE(LoadConfigFile(file_content, symlink_path.c_str()));
  std::filesystem::remove(symlink_path);
  std::filesystem::remove(target_path);
}

class ShardedMapTest : public ::testing::Test {};

TEST_F(ShardedMapTest, BasicSetAndGet) {
  ShardedMap<std::string, std::unique_ptr<int>> map;

  std::string key = "test_key";
  auto value = std::make_unique<int>(42);
  int* raw_ptr = value.get();

  map.Set(key, std::move(value));

  int* retrieved = map.Get(key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(*retrieved, 42);
  EXPECT_EQ(retrieved, raw_ptr);

  map.Unset(key);
}

TEST_F(ShardedMapTest, GetNonExistentKey) {
  ShardedMap<std::string, std::unique_ptr<int>> map;
  EXPECT_EQ(map.Get("non_existent"), nullptr);
}

TEST_F(ShardedMapTest, SetOverwritesExistingKey) {
  ShardedMap<std::string, std::unique_ptr<int>> map;

  std::string key = "test_key";
  auto value1 = std::make_unique<int>(100);
  auto value2 = std::make_unique<int>(200);
  int* raw_ptr2 = value2.get();

  map.Set(key, std::move(value1));
  map.Set(key, std::move(value2));

  int* retrieved = map.Get(key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(*retrieved, 200);
  EXPECT_EQ(retrieved, raw_ptr2);

  map.Unset(key);
}

TEST_F(ShardedMapTest, UnsetRemovesKey) {
  ShardedMap<std::string, std::unique_ptr<int>> map;

  std::string key = "test_key";
  auto value = std::make_unique<int>(42);

  map.Set(key, std::move(value));

  int* retrieved_before = map.Get(key);
  ASSERT_NE(retrieved_before, nullptr);

  map.Unset(key);

  EXPECT_EQ(map.Get(key), nullptr);
}

TEST_F(ShardedMapTest, UnsetNonExistentKey) {
  ShardedMap<std::string, std::unique_ptr<int>> map;
  EXPECT_NO_THROW(map.Unset("non_existent"));
}

TEST_F(ShardedMapTest, MultipleKeys) {
  ShardedMap<std::string, std::unique_ptr<int>> map;

  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    auto value = std::make_unique<int>(i * 10);
    map.Set(key, std::move(value));
  }

  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    int* value = map.Get(key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, i * 10);
  }

  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    map.Unset(key);
  }

  EXPECT_EQ(map.Get("key_5"), nullptr);
}

TEST_F(ShardedMapTest, DifferentShards) {
  ShardedMap<std::string, std::unique_ptr<int>> map;

  std::vector<std::string> keys = {"key1",        "key2", "key3", "another_key",
                                   "yet_another", "test", "data", "value"};

  for (size_t i = 0; i < keys.size(); i++) {
    auto value = std::make_unique<int>(i * 100);
    map.Set(keys[i], std::move(value));
  }

  for (size_t i = 0; i < keys.size(); i++) {
    int* value = map.Get(keys[i]);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, static_cast<int>(i * 100));
  }

  for (const auto& key : keys) {
    map.Unset(key);
  }
}

TEST_F(ShardedMapTest, ConcurrentOperations) {
  ShardedMap<std::string, std::unique_ptr<int>> map;

  for (int i = 0; i < 50; i++) {
    std::string key = "key_" + std::to_string(i);
    auto value = std::make_unique<int>(i);
    map.Set(key, std::move(value));
  }

  std::vector<std::thread> threads;

  // Reader threads
  for (int t = 0; t < 5; t++) {
    threads.emplace_back([&map]() {
      for (int i = 0; i < 100; i++) {
        std::string key = "key_" + std::to_string(i % 50);
        int* val = map.Get(key);
        ASSERT_NE(val, nullptr);
      }
    });
  }

  // Writer threads
  for (int t = 0; t < 5; t++) {
    threads.emplace_back([&map, t]() {
      for (int i = 0; i < 20; i++) {
        std::string key = "new_key_" + std::to_string(t * 20 + i);
        auto value = std::make_unique<int>(1000 + t * 20 + i);
        map.Set(key, std::move(value));
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // Original data should still be intact
  for (int i = 0; i < 50; i++) {
    std::string key = "key_" + std::to_string(i);
    int* value = map.Get(key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, i);
  }

  // Verify new data was written
  for (int t = 0; t < 5; t++) {
    for (int i = 0; i < 20; i++) {
      std::string key = "new_key_" + std::to_string(t * 20 + i);
      int* value = map.Get(key);
      ASSERT_NE(value, nullptr);
      EXPECT_EQ(*value, 1000 + t * 20 + i);
    }
  }

  for (int i = 0; i < 50; i++) {
    std::string key = "key_" + std::to_string(i);
    map.Unset(key);
  }
  for (int t = 0; t < 5; t++) {
    for (int i = 0; i < 20; i++) {
      std::string key = "new_key_" + std::to_string(t * 20 + i);
      map.Unset(key);
    }
  }
}

TEST_F(ShardedMapTest, IntegerKeys) {
  ShardedMap<int, std::unique_ptr<int>> map;

  for (int i = 0; i < 20; i++) {
    auto value = std::make_unique<int>(i * 5);
    map.Set(i, std::move(value));
  }

  for (int i = 0; i < 20; i++) {
    int* value = map.Get(i);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, i * 5);
  }

  for (int i = 0; i < 20; i++) {
    map.Unset(i);
  }

  // Verify cleanup
  EXPECT_EQ(map.Get(10), nullptr);
}

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
