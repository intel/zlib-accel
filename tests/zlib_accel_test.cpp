// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "../zlib_accel.h"

#include <gtest/gtest.h>
#include <stdio.h>

#include <cstdint>
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

#ifdef USE_IGZIP
#include "../igzip.h"
#endif

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

int ZlibCompressWithLevel(const char* input, size_t input_length,
                          std::string* output, int level, int window_bits,
                          int flush, size_t* output_upper_bound,
                          ExecutionPath* execution_path) {
  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));

  int st = deflateInit2(&stream, level, Z_DEFLATED, window_bits, 8,
                        Z_DEFAULT_STRATEGY);
  if (st != Z_OK) {
    deflateEnd(&stream);
    return st;
  }

  stream.next_in = (Bytef*)input;
  stream.avail_in = static_cast<unsigned int>(input_length);

  *output_upper_bound =
      deflateBound(&stream, static_cast<unsigned long>(input_length));
  output->resize(*output_upper_bound);
  stream.avail_out = static_cast<unsigned int>(*output_upper_bound);
  stream.next_out = reinterpret_cast<Bytef*>(&(*output)[0]);

  st = deflate(&stream, flush);
  *execution_path = GetDeflateExecutionPath(&stream);
  if (st != Z_STREAM_END) {
    deflateEnd(&stream);
    return st;
  }
  output->resize(stream.total_out);

  deflateEnd(&stream);
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
      SetConfig(USE_IGZIP_COMPRESS, 0);
      SetConfig(USE_QAT_COMPRESS, 0);
      SetConfig(USE_ZLIB_COMPRESS, 1);
      break;
    case QAT:
      SetConfig(USE_IAA_COMPRESS, 0);
      SetConfig(USE_IGZIP_COMPRESS, 0);
      SetConfig(USE_QAT_COMPRESS, 1);
      SetConfig(USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IAA:
      SetConfig(USE_IAA_COMPRESS, 1);
      SetConfig(USE_IGZIP_COMPRESS, 0);
      SetConfig(USE_QAT_COMPRESS, 0);
      SetConfig(USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IGZIP:
      SetConfig(USE_IGZIP_COMPRESS, 1);
      SetConfig(USE_IAA_COMPRESS, 0);
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
      SetConfig(USE_IGZIP_UNCOMPRESS, 0);
      SetConfig(USE_QAT_UNCOMPRESS, 0);
      SetConfig(USE_ZLIB_UNCOMPRESS, 1);
      break;
    case QAT:
      SetConfig(USE_IAA_UNCOMPRESS, 0);
      SetConfig(USE_IGZIP_UNCOMPRESS, 0);
      SetConfig(USE_QAT_UNCOMPRESS, 1);
      SetConfig(USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IAA:
      SetConfig(USE_IAA_UNCOMPRESS, 1);
      SetConfig(USE_IGZIP_UNCOMPRESS, 0);
      SetConfig(USE_QAT_UNCOMPRESS, 0);
      SetConfig(USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IGZIP:
      SetConfig(USE_IAA_UNCOMPRESS, 0);
      SetConfig(USE_IGZIP_UNCOMPRESS, 1);
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
      case IGZIP:
        return "IGZIP";
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

  // For IGZIP->IAA compatibility checks, force max zlib level so IGZIP uses
  // ISA-L level 3 (stricter match selection), which makes long-history
  // limitations consistently observable.
  const int compression_level = (test_param.execution_path_compress == IGZIP &&
                                 test_param.execution_path_uncompress == IAA)
                                    ? 9
                                    : -1;

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompressWithLevel(
      input, input_length, &compressed, compression_level,
      test_param.window_bits_compress, test_param.flush_compress,
      &output_upper_bound, &execution_path);
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
      } else if (test_param.execution_path_compress == IGZIP) {
        VerifyStatIncremented(Statistic::DEFLATE_IGZIP_COUNT);
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
      } else if (test_param.execution_path_uncompress == IGZIP) {
        VerifyStatIncrementedUpTo(Statistic::INFLATE_IGZIP_COUNT,
                                  test_param.input_chunks_uncompress);
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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

  // For IGZIP->IAA compatibility checks, force max zlib level so IGZIP uses
  // ISA-L level 3 (stricter match selection), which makes long-history
  // limitations consistently observable.
  const int compression_level = (test_param.execution_path_compress == IGZIP &&
                                 test_param.execution_path_uncompress == IAA)
                                    ? 9
                                    : -1;

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompressWithLevel(
      input, input_length, &compressed, compression_level,
      test_param.window_bits_compress, test_param.flush_compress,
      &output_upper_bound, &execution_path);

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

  // zlib and igzip decompression may return partial progress instead of error
  if (test_param.execution_path_uncompress == ZLIB ||
      test_param.execution_path_uncompress == IGZIP ||
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

  // For IGZIP->IAA compatibility checks, force max zlib level so IGZIP uses
  // ISA-L level 3 (stricter match selection), which makes long-history
  // limitations consistently observable.
  const int compression_level = (test_param.execution_path_compress == IGZIP &&
                                 test_param.execution_path_uncompress == IAA)
                                    ? 9
                                    : -1;

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  // Compress data in 2 streams
  std::string compressed1;
  size_t input_length1 = input_length / 2;
  size_t output_upper_bound1;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompressWithLevel(
      input, input_length1, &compressed1, compression_level,
      test_param.window_bits_compress, test_param.flush_compress,
      &output_upper_bound1, &execution_path);

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
  ret = ZlibCompressWithLevel(
      input + input_length1, input_length2, &compressed2, compression_level,
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

    // IAA/IGZIP may consume bytes beyond first-stream boundary
    if (test_param.execution_path_uncompress != IAA &&
        test_param.execution_path_uncompress != IGZIP) {
      ASSERT_EQ(input_consumed, compressed1.length());
    }
  }

  delete[] uncompressed;
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST(IGZIPInflateRegressionTest, EmptyInputContinuationKeepsIGZIPPath) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  const size_t input_length = 1 << 20;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &execution_path);
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(execution_path, ZLIB);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

  std::vector<char> output_chunk(8192);
  stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  stream.avail_in = static_cast<unsigned int>(compressed.size());

  int iter = 0;
  int last_ret = Z_OK;
  while (iter++ < 2048) {
    stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
    stream.avail_out = static_cast<unsigned int>(output_chunk.size());
    last_ret = inflate(&stream, Z_SYNC_FLUSH);
    ASSERT_NE(last_ret, Z_DATA_ERROR);
    ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);

    if (last_ret == Z_STREAM_END) {
      break;
    }

    if (stream.avail_in == 0 && last_ret == Z_OK) {
      break;
    }
  }

  ASSERT_NE(last_ret, Z_STREAM_END);
  ASSERT_EQ(stream.avail_in, 0u);

  stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
  stream.avail_out = static_cast<unsigned int>(output_chunk.size());
  stream.next_in = nullptr;
  stream.avail_in = 0;

  int continuation_ret = inflate(&stream, Z_SYNC_FLUSH);
  EXPECT_TRUE(continuation_ret == Z_BUF_ERROR || continuation_ret == Z_OK ||
              continuation_ret == Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP);

  inflateEnd(&stream);
  DestroyBlock(input);
}

TEST(IGZIPInflateRegressionTest, RawContinuationMustNotIncreaseAvailIn) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  const size_t input_length = 16384;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &execution_path);
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(execution_path, ZLIB);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

  std::vector<char> output_chunk(8192);
  stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  stream.avail_in = static_cast<unsigned int>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
  stream.avail_out = static_cast<unsigned int>(output_chunk.size());

  ret = inflate(&stream, Z_SYNC_FLUSH);
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);
  ASSERT_TRUE(ret == Z_OK || ret == Z_STREAM_END);

  while (ret == Z_OK && stream.avail_in > 0) {
    stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
    stream.avail_out = static_cast<unsigned int>(output_chunk.size());
    ret = inflate(&stream, Z_SYNC_FLUSH);
    ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);
    ASSERT_TRUE(ret == Z_OK || ret == Z_STREAM_END);
  }

  ASSERT_EQ(stream.avail_in, 0u);

  stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
  stream.avail_out = static_cast<unsigned int>(output_chunk.size());
  stream.next_in = nullptr;
  stream.avail_in = 0;
  ret = inflate(&stream, Z_SYNC_FLUSH);
  ASSERT_TRUE(ret == Z_BUF_ERROR || ret == Z_OK || ret == Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);

  uint8_t one_byte = 0;
  stream.next_in = &one_byte;
  stream.avail_in = 1;
  stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
  stream.avail_out = static_cast<unsigned int>(output_chunk.size());

  ret = inflate(&stream, Z_SYNC_FLUSH);
  EXPECT_NE(ret, Z_DATA_ERROR);
  EXPECT_LE(stream.avail_in, 1u);

  inflateEnd(&stream);
  DestroyBlock(input);
}

TEST(IGZIPInflateRegressionTest, RawTrailingByteMustNotIncreaseAvailIn) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  const size_t input_length = 4096;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &execution_path);
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(execution_path, ZLIB);

  std::string compressed_with_trailing = compressed;
  compressed_with_trailing.push_back('\0');

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

  std::vector<char> uncompressed(input_length * 2);
  stream.next_in = reinterpret_cast<Bytef*>(compressed_with_trailing.data());
  stream.avail_in = static_cast<unsigned int>(compressed_with_trailing.size());
  stream.next_out = reinterpret_cast<Bytef*>(uncompressed.data());
  stream.avail_out = static_cast<unsigned int>(uncompressed.size());

  ret = inflate(&stream, Z_SYNC_FLUSH);
  EXPECT_NE(ret, Z_DATA_ERROR);
  EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP);

  // For valid raw-deflate stream with one extra byte, inflate may leave that
  // byte unconsumed, but avail_in must never increase.
  EXPECT_LE(stream.avail_in, 1u);

  inflateEnd(&stream);
  DestroyBlock(input);
}

TEST(IGZIPInflateRegressionTest,
     RawOneByteContinuationMustNotIncreaseAvailInAcrossSizes) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  std::vector<size_t> input_sizes = {1,    2,    3,    7,    8,     15,   16,
                                     31,   32,   63,   64,   127,   128,  255,
                                     256,  511,  512,  1023, 1024,  2047, 2048,
                                     4095, 4096, 8191, 8192, 16384, 32768};

  for (size_t input_length : input_sizes) {
    std::vector<char> input(input_length);
    for (size_t i = 0; i < input_length; ++i) {
      input[i] = static_cast<char>((i * 131u + 17u) & 0xFFu);
    }

    std::string compressed;
    size_t output_upper_bound;
    ExecutionPath execution_path = UNDEFINED;
    int ret = ZlibCompress(input.data(), input_length, &compressed, -15,
                           Z_FINISH, &output_upper_bound, &execution_path);
    ASSERT_EQ(ret, Z_STREAM_END);
    ASSERT_EQ(execution_path, ZLIB);

    z_stream stream;
    memset(&stream, 0, sizeof(z_stream));
    ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

    std::vector<char> output_chunk(1024);
    stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<unsigned int>(compressed.size());

    int last_ret = Z_OK;
    for (int iter = 0; iter < 4096; ++iter) {
      stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
      stream.avail_out = static_cast<unsigned int>(output_chunk.size());
      last_ret = inflate(&stream, Z_SYNC_FLUSH);
      ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);
      ASSERT_NE(last_ret, Z_DATA_ERROR);

      if (stream.avail_in == 0 || last_ret == Z_STREAM_END) {
        break;
      }
    }

    ASSERT_EQ(stream.avail_in, 0u);

    stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
    stream.avail_out = static_cast<unsigned int>(output_chunk.size());
    stream.next_in = nullptr;
    stream.avail_in = 0;
    int empty_ret = inflate(&stream, Z_SYNC_FLUSH);
    EXPECT_TRUE(empty_ret == Z_BUF_ERROR || empty_ret == Z_OK ||
                empty_ret == Z_STREAM_END);
    EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP);

    uint8_t trailing_byte = 0;
    stream.next_in = &trailing_byte;
    stream.avail_in = 1;
    stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
    stream.avail_out = static_cast<unsigned int>(output_chunk.size());

    int one_byte_ret = inflate(&stream, Z_SYNC_FLUSH);
    EXPECT_NE(one_byte_ret, Z_DATA_ERROR);
    EXPECT_LE(stream.avail_in, 1u) << "input_length=" << input_length;

    inflateEnd(&stream);
  }
}

TEST(IGZIPInflateRegressionTest,
     TinyRawEntryMustNotOverconsumePastStreamBoundary) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  // Empty raw-deflate entries are often tiny (commonly 2 bytes). The
  // decompressor must stop exactly at stream end and leave trailing bytes for
  // the caller.
  const std::string empty_payload;
  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret =
      ZlibCompress(empty_payload.data(), empty_payload.size(), &compressed, -15,
                   Z_FINISH, &output_upper_bound, &execution_path);
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(execution_path, ZLIB);
  ASSERT_GT(compressed.size(), 0u);

  const size_t trailing_len = 510;
  std::string input = compressed;
  input.append(trailing_len, static_cast<char>(0xA5));

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

  std::vector<char> output(512);
  stream.next_in = reinterpret_cast<Bytef*>(input.data());
  stream.avail_in = static_cast<unsigned int>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<unsigned int>(output.size());

  ret = inflate(&stream, Z_SYNC_FLUSH);
  const ExecutionPath observed_path = GetInflateExecutionPath(&stream);
  ASSERT_TRUE(observed_path == IGZIP || observed_path == ZLIB);
  ASSERT_NE(ret, Z_DATA_ERROR);

  // No output is expected for empty payload. Most importantly, all trailing
  // bytes must remain unconsumed for the caller.
  EXPECT_EQ(output.size() - stream.avail_out, 0u);
  EXPECT_EQ(stream.avail_in, trailing_len)
      << "compressed_size=" << compressed.size();

  inflateEnd(&stream);
}

TEST(IGZIPInflateRegressionTest, RawStreamEndMustPreserveEightTrailingBytes) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  const size_t input_length = 32768;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &execution_path);
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(execution_path, ZLIB);

  std::string with_trailing = compressed;
  with_trailing.append("ABCDEFGH", 8);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

  std::vector<char> output(input_length * 2);
  stream.next_in = reinterpret_cast<Bytef*>(with_trailing.data());
  stream.avail_in = static_cast<unsigned int>(with_trailing.size());

  int last_ret = Z_OK;
  for (int iter = 0; iter < 64; ++iter) {
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());
    last_ret = inflate(&stream, Z_SYNC_FLUSH);
    const ExecutionPath path = GetInflateExecutionPath(&stream);
    ASSERT_TRUE(path == IGZIP || path == ZLIB);
    ASSERT_NE(last_ret, Z_DATA_ERROR);
    if (last_ret == Z_STREAM_END) {
      break;
    }
    if (stream.avail_out > 0 && stream.avail_in == 0) {
      break;
    }
  }

  EXPECT_EQ(last_ret, Z_STREAM_END);
  EXPECT_EQ(stream.avail_in, 8u);

  inflateEnd(&stream);
  DestroyBlock(input);
}

TEST(IGZIPInflateRegressionTest, RawSplitInputDefersCorrectionUntilStreamEnd) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  const size_t input_length = 65536;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &execution_path);
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(execution_path, ZLIB);
  ASSERT_GT(compressed.size(), 2u);

  std::string with_trailing = compressed;
  with_trailing.append("ABCDEFGH", 8);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

  std::vector<char> output(input_length * 2);
  const size_t split_offset = compressed.size() - 1;

  stream.next_in = reinterpret_cast<Bytef*>(with_trailing.data());
  stream.avail_in = static_cast<unsigned int>(split_offset);
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<unsigned int>(output.size());

  ret = inflate(&stream, Z_SYNC_FLUSH);
  {
    const ExecutionPath path = GetInflateExecutionPath(&stream);
    ASSERT_TRUE(path == IGZIP || path == ZLIB);
  }
  ASSERT_NE(ret, Z_DATA_ERROR);

  stream.next_in =
      reinterpret_cast<Bytef*>(with_trailing.data() + split_offset);
  stream.avail_in =
      static_cast<unsigned int>(with_trailing.size() - split_offset);

  int last_ret = ret;
  for (int iter = 0; iter < 64; ++iter) {
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());
    last_ret = inflate(&stream, Z_SYNC_FLUSH);
    const ExecutionPath path = GetInflateExecutionPath(&stream);
    ASSERT_TRUE(path == IGZIP || path == ZLIB);
    ASSERT_NE(last_ret, Z_DATA_ERROR);
    if (last_ret == Z_STREAM_END) {
      break;
    }
    if (stream.avail_out > 0 && stream.avail_in == 0) {
      break;
    }
  }

  EXPECT_EQ(last_ret, Z_STREAM_END);
  EXPECT_EQ(stream.avail_in, 8u);

  inflateEnd(&stream);
  DestroyBlock(input);
}

TEST(IGZIPDeflateRegressionTest,
     RepeatedFinishWithEmptyInputMustReturnStreamEnd) {
  SetCompressPath(IGZIP, true, false, false);
  SetUncompressPath(ZLIB, false, false);
  if (GetConfig(USE_ZLIB_COMPRESS) == 0) {
    GTEST_SKIP() << "USE_ZLIB_COMPRESS=0 disables fallback-first contract";
  }

  const char* input = "igzip-finish-regression-payload";
  const size_t input_length = strlen(input);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<char> output(4096);
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input));
  stream.avail_in = static_cast<unsigned int>(input_length);

  int ret = Z_OK;
  for (int iter = 0; iter < 32; ++iter) {
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());
    ret = deflate(&stream, Z_FINISH);
    const ExecutionPath path = GetDeflateExecutionPath(&stream);
    ASSERT_TRUE(path == IGZIP || path == ZLIB);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  ASSERT_EQ(ret, Z_STREAM_END);

  for (int iter = 0; iter < 64; ++iter) {
    stream.next_in = nullptr;
    stream.avail_in = 0;
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());

    int finish_ret = deflate(&stream, Z_FINISH);
    EXPECT_EQ(finish_ret, Z_STREAM_END) << "iter=" << iter;
    const ExecutionPath path = GetDeflateExecutionPath(&stream);
    EXPECT_TRUE(path == IGZIP || path == ZLIB) << "iter=" << iter;
  }

  deflateEnd(&stream);
}

TEST(IGZIPDeflateRegressionTest, ResetMustNotStallSyncFlushOnSameStream) {
  SetCompressPath(IGZIP, true, false, false);
  SetUncompressPath(ZLIB, false, false);
  if (GetConfig(USE_ZLIB_COMPRESS) == 0) {
    GTEST_SKIP() << "USE_ZLIB_COMPRESS=0 disables fallback-first contract";
  }

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<char> output(4096);
  std::vector<char> input(512, 'A');

  for (int cycle = 0; cycle < 20; ++cycle) {
    stream.next_in = reinterpret_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<unsigned int>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());

    int ret = deflate(&stream, Z_NO_FLUSH);
    ASSERT_TRUE(ret == Z_OK || ret == Z_BUF_ERROR) << "cycle=" << cycle;
    ASSERT_EQ(GetDeflateExecutionPath(&stream), IGZIP) << "cycle=" << cycle;

    stream.next_in = nullptr;
    stream.avail_in = 0;
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());

    ret = deflate(&stream, Z_SYNC_FLUSH);
    ASSERT_EQ(ret, Z_OK) << "cycle=" << cycle;
    ASSERT_LT(stream.avail_out, output.size()) << "cycle=" << cycle;
    ASSERT_EQ(GetDeflateExecutionPath(&stream), IGZIP) << "cycle=" << cycle;

    stream.next_in = nullptr;
    stream.avail_in = 0;
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());

    ret = deflate(&stream, Z_FINISH);
    ASSERT_EQ(ret, Z_STREAM_END) << "cycle=" << cycle;

    ASSERT_EQ(deflateReset(&stream), Z_OK) << "cycle=" << cycle;
  }

  deflateEnd(&stream);
}

TEST(IGZIPDeflateRegressionTest,
     RepeatedEmptySyncFlushMustEventuallyReportNoProgress) {
  SetCompressPath(IGZIP, true, false, false);
  SetUncompressPath(ZLIB, false, false);
  if (GetConfig(USE_ZLIB_COMPRESS) == 0) {
    GTEST_SKIP() << "USE_ZLIB_COMPRESS=0 disables fallback-first contract";
  }

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<char> output(4096);
  std::vector<char> input(1024, 'B');

  stream.next_in = reinterpret_cast<Bytef*>(input.data());
  stream.avail_in = static_cast<unsigned int>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<unsigned int>(output.size());
  int ret = deflate(&stream, Z_NO_FLUSH);
  ASSERT_TRUE(ret == Z_OK || ret == Z_BUF_ERROR);
  ASSERT_EQ(GetDeflateExecutionPath(&stream), IGZIP);

  bool observed_buf_error = false;
  for (int iter = 0; iter < 128; ++iter) {
    stream.next_in = nullptr;
    stream.avail_in = 0;
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());

    ret = deflate(&stream, Z_SYNC_FLUSH);
    ASSERT_EQ(GetDeflateExecutionPath(&stream), IGZIP) << "iter=" << iter;
    ASSERT_NE(ret, Z_DATA_ERROR) << "iter=" << iter;

    if (ret == Z_BUF_ERROR) {
      observed_buf_error = true;
      break;
    }
  }

  EXPECT_TRUE(observed_buf_error)
      << "Repeated empty Z_SYNC_FLUSH calls must eventually stop producing "
      << "new bytes and return Z_BUF_ERROR";

  stream.next_in = nullptr;
  stream.avail_in = 0;
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<unsigned int>(output.size());
  ret = deflate(&stream, Z_FINISH);
  EXPECT_EQ(ret, Z_STREAM_END);

  deflateEnd(&stream);
}

// Regression test for: deflateReset on a reused IGZIP stream must restore the
// zlib header (gzip_flag = IGZIP_ZLIB) so that each independent chunk is
// self-contained and decompressible by a fresh zlib inflater.  Without the
// fix, isal_deflate_reset preserved gzip_flag = IGZIP_ZLIB_NO_HDR (4) and the
// second and subsequent chunks were emitted without a zlib header, causing
// Java's Inflater (nowrap=false) to report "incorrect header check".
TEST(IGZIPDeflateRegressionTest,
     ResetMustRestoreZlibHeaderForSubsequentChunks) {
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(ZLIB, false, false);

  z_stream cstream;
  memset(&cstream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         /*windowBits=*/15, /*memLevel=*/8, Z_DEFAULT_STRATEGY),
            Z_OK);

  const int kChunks = 5;
  const int kChunkSize = 16384;
  const int kCompBound = deflateBound(&cstream, kChunkSize);

  for (int chunk = 0; chunk < kChunks; ++chunk) {
    // Each chunk is independent data compressed with a fresh IGZIP stream
    // (via deflateReset).
    std::vector<uint8_t> input(kChunkSize,
                               static_cast<uint8_t>('A' + chunk % 26));
    std::vector<uint8_t> compressed(kCompBound);

    cstream.next_in = input.data();
    cstream.avail_in = static_cast<uInt>(input.size());
    cstream.next_out = compressed.data();
    cstream.avail_out = static_cast<uInt>(compressed.size());

    ASSERT_EQ(deflate(&cstream, Z_FINISH), Z_STREAM_END) << "chunk=" << chunk;
    ASSERT_EQ(GetDeflateExecutionPath(&cstream), IGZIP) << "chunk=" << chunk;

    const size_t compressed_size = compressed.size() - cstream.avail_out;

    // Decompress with a fresh zlib inflater — requires a valid zlib header.
    std::vector<uint8_t> decompressed(kChunkSize);
    z_stream dstream;
    memset(&dstream, 0, sizeof(z_stream));
    ASSERT_EQ(inflateInit(&dstream), Z_OK) << "chunk=" << chunk;
    dstream.next_in = compressed.data();
    dstream.avail_in = static_cast<uInt>(compressed_size);
    dstream.next_out = decompressed.data();
    dstream.avail_out = static_cast<uInt>(decompressed.size());
    ASSERT_EQ(inflate(&dstream, Z_FINISH), Z_STREAM_END)
        << "chunk=" << chunk
        << ": decompression failed (missing zlib header after deflateReset?)";
    ASSERT_EQ(decompressed, input) << "chunk=" << chunk;
    inflateEnd(&dstream);

    ASSERT_EQ(deflateReset(&cstream), Z_OK) << "chunk=" << chunk;
  }

  deflateEnd(&cstream);
}

// Regression: after inflate() returns Z_STREAM_END and avail_in is left
// pointing at trailing bytes (Bug 1 fix), a subsequent inflate() call with
// those trailing bytes must return Z_STREAM_END, not Z_BUF_ERROR. The IGZIP
// stream is still active (not freed), and isal_inflate on a finished stream
// returns ISAL_END_INPUT with 0 consumed — the avail_in>0 path in inflate()
// previously fell through to Z_BUF_ERROR when input_len==output_len==0.
TEST(IGZIPInflateRegressionTest,
     InflateAfterStreamEndWithTrailingBytesReturnsStreamEnd) {
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  // Compress with IGZIP (raw deflate).
  z_stream cstream;
  memset(&cstream, 0, sizeof(z_stream));
  const size_t kPayloadSize = 4096;
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  std::vector<uint8_t> payload(kPayloadSize, 'Q');
  std::vector<uint8_t> compressed(deflateBound(&cstream, kPayloadSize));
  cstream.next_in = payload.data();
  cstream.avail_in = static_cast<uInt>(kPayloadSize);
  cstream.next_out = compressed.data();
  cstream.avail_out = static_cast<uInt>(compressed.size());
  ASSERT_EQ(deflate(&cstream, Z_FINISH), Z_STREAM_END);
  ASSERT_EQ(GetDeflateExecutionPath(&cstream), IGZIP);
  const size_t compressed_size = compressed.size() - cstream.avail_out;
  deflateEnd(&cstream);

  // Build input buffer: compressed stream + 8 trailing bytes.
  std::vector<uint8_t> input_buf(compressed.begin(),
                                 compressed.begin() + compressed_size);
  input_buf.insert(input_buf.end(), {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'});

  z_stream dstream;
  memset(&dstream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&dstream, -15), Z_OK);
  std::vector<uint8_t> output(kPayloadSize * 2);

  dstream.next_in = input_buf.data();
  dstream.avail_in = static_cast<uInt>(input_buf.size());
  dstream.next_out = output.data();
  dstream.avail_out = static_cast<uInt>(output.size());

  // First call: IGZIP decompresses the stream and leaves 8 trailing bytes.
  ASSERT_EQ(inflate(&dstream, Z_SYNC_FLUSH), Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&dstream), IGZIP);
  ASSERT_EQ(dstream.avail_in, 8u) << "Bug 1 fix should rewind trailing bytes";

  // Second call: avail_in==8 > 0, IGZIP stream still active. isal_inflate on
  // a finished stream produces no I/O — must return Z_STREAM_END, not
  // Z_BUF_ERROR.
  dstream.next_out = output.data();
  dstream.avail_out = static_cast<uInt>(output.size());
  EXPECT_EQ(inflate(&dstream, Z_SYNC_FLUSH), Z_STREAM_END);

  inflateEnd(&dstream);
}

// Regression: after inflate() returns Z_STREAM_END with avail_in==0, a
// subsequent inflate() call with avail_in==0 (IGZIPHandleActiveStreamNoInput
// path) must return Z_STREAM_END, not Z_BUF_ERROR.
TEST(IGZIPInflateRegressionTest,
     InflateWithZeroInputAfterStreamEndReturnsStreamEnd) {
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  z_stream cstream;
  memset(&cstream, 0, sizeof(z_stream));
  const size_t kPayloadSize = 4096;
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  std::vector<uint8_t> payload(kPayloadSize, 'Q');
  std::vector<uint8_t> compressed(deflateBound(&cstream, kPayloadSize));
  cstream.next_in = payload.data();
  cstream.avail_in = static_cast<uInt>(kPayloadSize);
  cstream.next_out = compressed.data();
  cstream.avail_out = static_cast<uInt>(compressed.size());
  ASSERT_EQ(deflate(&cstream, Z_FINISH), Z_STREAM_END);
  ASSERT_EQ(GetDeflateExecutionPath(&cstream), IGZIP);
  const size_t compressed_size = compressed.size() - cstream.avail_out;
  deflateEnd(&cstream);

  z_stream dstream;
  memset(&dstream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&dstream, -15), Z_OK);
  std::vector<uint8_t> output(kPayloadSize * 2);

  dstream.next_in = compressed.data();
  dstream.avail_in = static_cast<uInt>(compressed_size);
  dstream.next_out = output.data();
  dstream.avail_out = static_cast<uInt>(output.size());

  // First call: consumes all compressed bytes exactly, avail_in drops to 0.
  ASSERT_EQ(inflate(&dstream, Z_SYNC_FLUSH), Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&dstream), IGZIP);
  ASSERT_EQ(dstream.avail_in, 0u);

  // Second call: avail_in==0, IGZIP stream still active
  // (IGZIPHandleActiveStreamNoInput path). isal_inflate on a finished stream
  // with no input produces no output — must return Z_STREAM_END, not
  // Z_BUF_ERROR.
  dstream.next_out = output.data();
  dstream.avail_out = static_cast<uInt>(output.size());
  EXPECT_EQ(inflate(&dstream, Z_SYNC_FLUSH), Z_STREAM_END);

  inflateEnd(&dstream);
}

// inflateReset() reuses the already-allocated ISA-L stream, so ConfigureInflate
// Window() is not called again for the second stream.  That is only correct
// because isal_inflate_reset() leaves crc_flag (the wrapper format) and
// hist_bits alone -- isal_inflate_init() zeroes both, reset deliberately does
// not touch them.  igzip_lib.h documents no guarantee either way, so guard the
// behavior here: with zlib_fallback off, a regression surfaces directly as
// Z_DATA_ERROR on the second stream instead of being masked by zlib taking
// over.
TEST(IGZIPInflateRegressionTest, ResetMustPreserveWrapperFormatAcrossStreams) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  // 31 = gzip wrapper, -15 = raw deflate.  A cleared crc_flag would decode the
  // gzip case as raw deflate; a cleared hist_bits would corrupt long matches.
  for (const int window_bits : {31, -15}) {
    const size_t input_length = 16384;
    char* first = GenerateBlock(input_length, compressible_block);
    ASSERT_NE(first, nullptr) << "window_bits " << window_bits;
    char* second = GenerateBlock(input_length, incompressible_block);
    ASSERT_NE(second, nullptr) << "window_bits " << window_bits;

    std::string first_compressed;
    std::string second_compressed;
    size_t output_upper_bound = 0;
    ExecutionPath compress_path = UNDEFINED;
    ASSERT_EQ(ZlibCompress(first, input_length, &first_compressed, window_bits,
                           Z_FINISH, &output_upper_bound, &compress_path),
              Z_STREAM_END)
        << "window_bits " << window_bits;
    ASSERT_EQ(
        ZlibCompress(second, input_length, &second_compressed, window_bits,
                     Z_FINISH, &output_upper_bound, &compress_path),
        Z_STREAM_END)
        << "window_bits " << window_bits;

    z_stream dstream;
    memset(&dstream, 0, sizeof(dstream));
    ASSERT_EQ(inflateInit2(&dstream, window_bits), Z_OK)
        << "window_bits " << window_bits;

    // Both streams are decompressed on the same z_stream, with only an
    // inflateReset() in between.
    const std::string* compressed[2] = {&first_compressed, &second_compressed};
    const char* expected[2] = {first, second};
    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1) {
        ASSERT_EQ(inflateReset(&dstream), Z_OK)
            << "window_bits " << window_bits;
      }

      std::vector<unsigned char> output(input_length + 1024);
      dstream.next_in = reinterpret_cast<Bytef*>(
          const_cast<char*>(compressed[stream_index]->data()));
      dstream.avail_in = static_cast<uInt>(compressed[stream_index]->size());
      dstream.next_out = output.data();
      dstream.avail_out = static_cast<uInt>(output.size());

      int ret = Z_OK;
      for (int iter = 0; iter < 1024; ++iter) {
        ret = inflate(&dstream, Z_NO_FLUSH);
        ASSERT_NE(ret, Z_DATA_ERROR)
            << "window_bits " << window_bits << ", stream " << stream_index;
        if (ret == Z_STREAM_END) {
          break;
        }
        if (ret == Z_BUF_ERROR && dstream.avail_in == 0) {
          break;
        }
      }
      ASSERT_EQ(ret, Z_STREAM_END)
          << "window_bits " << window_bits << ", stream " << stream_index;
      EXPECT_EQ(GetInflateExecutionPath(&dstream), IGZIP)
          << "window_bits " << window_bits << ", stream " << stream_index;

      const size_t produced = output.size() - dstream.avail_out;
      ASSERT_EQ(produced, input_length)
          << "window_bits " << window_bits << ", stream " << stream_index;
      EXPECT_EQ(memcmp(output.data(), expected[stream_index], input_length), 0)
          << "window_bits " << window_bits << ", stream " << stream_index;
    }

    inflateEnd(&dstream);
    DestroyBlock(first);
    DestroyBlock(second);
  }
}

TEST(IGZIPDeflateRegressionTest, DictionaryStreamMustStayOnZlibAcrossReset) {
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(ZLIB, false, false);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  const char dict[] = "0123456789abcdef";
  ASSERT_EQ(deflateSetDictionary(&stream, reinterpret_cast<const Bytef*>(dict),
                                 static_cast<uInt>(sizeof(dict) - 1)),
            Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);

  std::vector<char> out1(4096);
  const char* msg1 = "dictionary-stream-first-message";
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(msg1));
  stream.avail_in = static_cast<unsigned int>(strlen(msg1));
  int ret = Z_OK;
  for (int iter = 0; iter < 16; ++iter) {
    stream.next_out = reinterpret_cast<Bytef*>(out1.data());
    stream.avail_out = static_cast<unsigned int>(out1.size());
    ret = deflate(&stream, Z_FINISH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);
  const size_t out1_size = out1.size() - stream.avail_out;

  ASSERT_EQ(deflateReset(&stream), Z_OK);

  // Re-set dictionary after reset (standard zlib API usage).
  ASSERT_EQ(deflateSetDictionary(&stream, reinterpret_cast<const Bytef*>(dict),
                                 static_cast<uInt>(sizeof(dict) - 1)),
            Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);

  std::vector<char> out2(4096);
  const char* msg2 = "dictionary-stream-second-message";
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(msg2));
  stream.avail_in = static_cast<unsigned int>(strlen(msg2));
  ret = Z_OK;
  for (int iter = 0; iter < 16; ++iter) {
    stream.next_out = reinterpret_cast<Bytef*>(out2.data());
    stream.avail_out = static_cast<unsigned int>(out2.size());
    ret = deflate(&stream, Z_FINISH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);
  const size_t out2_size = out2.size() - stream.avail_out;

  z_stream verify;
  memset(&verify, 0, sizeof(verify));
  ASSERT_EQ(inflateInit2(&verify, 15), Z_OK);

  std::vector<char> verify_out(1024);
  verify.next_in = reinterpret_cast<Bytef*>(out2.data());
  verify.avail_in = static_cast<unsigned int>(out2_size);
  verify.next_out = reinterpret_cast<Bytef*>(verify_out.data());
  verify.avail_out = static_cast<unsigned int>(verify_out.size());

  ret = inflate(&verify, Z_NO_FLUSH);
  EXPECT_EQ(ret, Z_NEED_DICT);

  ASSERT_EQ(inflateSetDictionary(&verify, reinterpret_cast<const Bytef*>(dict),
                                 static_cast<uInt>(sizeof(dict) - 1)),
            Z_OK);
  ret = inflate(&verify, Z_FINISH);
  EXPECT_EQ(ret, Z_STREAM_END);

  inflateEnd(&verify);

  EXPECT_GT(out1_size, 0u);
  EXPECT_GT(out2_size, 0u);

  deflateEnd(&stream);
}

TEST(IGZIPDeflateRegressionTest,
     ParallelDictionaryResetStreamsRemainRoundTripSafe) {
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(ZLIB, false, false);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  const std::string dictionary =
      "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const int thread_count = 4;
  const int iterations_per_thread = 120;

  std::atomic<bool> failed{false};
  std::string failure_reason;
  std::mutex failure_mutex;

  auto set_failure = [&](const std::string& message) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      std::lock_guard<std::mutex> lock(failure_mutex);
      failure_reason = message;
    }
  };

  auto worker = [&](int worker_id) {
    z_stream cstream;
    memset(&cstream, 0, sizeof(cstream));
    int ret = deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                           Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
      set_failure("deflateInit2 failed");
      return;
    }

    for (int i = 0; i < iterations_per_thread && !failed.load(); ++i) {
      if (deflateReset(&cstream) != Z_OK) {
        set_failure("deflateReset failed");
        break;
      }

      ret = deflateSetDictionary(
          &cstream, reinterpret_cast<const Bytef*>(dictionary.data()),
          static_cast<uInt>(dictionary.size()));
      if (ret != Z_OK) {
        set_failure("deflateSetDictionary failed");
        break;
      }

      if (GetDeflateExecutionPath(&cstream) != ZLIB) {
        set_failure("dictionary stream did not stay on zlib");
        break;
      }

      std::string input;
      input.reserve(64 * 1024);
      for (int j = 0; j < 1024; ++j) {
        input += "tid=";
        input += std::to_string(worker_id);
        input += " iter=";
        input += std::to_string(i);
        input += " row=";
        input += std::to_string(j);
        input += " payload=";
        input += dictionary;
        input += "\n";
      }

      std::vector<unsigned char> compressed(compressBound(input.size()));
      cstream.next_in =
          reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
      cstream.avail_in = static_cast<unsigned int>(input.size());
      cstream.next_out = compressed.data();
      cstream.avail_out = static_cast<unsigned int>(compressed.size());

      int def_ret = Z_OK;
      for (int guard = 0; guard < 64; ++guard) {
        def_ret = deflate(&cstream, Z_FINISH);
        if (def_ret == Z_STREAM_END) {
          break;
        }
        if (def_ret != Z_OK && def_ret != Z_BUF_ERROR) {
          break;
        }
      }
      if (def_ret != Z_STREAM_END) {
        set_failure("deflate did not reach Z_STREAM_END");
        break;
      }

      const size_t compressed_size = compressed.size() - cstream.avail_out;
      if (compressed_size == 0) {
        set_failure("compressed payload was empty");
        break;
      }

      z_stream dstream;
      memset(&dstream, 0, sizeof(dstream));
      ret = inflateInit2(&dstream, 15);
      if (ret != Z_OK) {
        set_failure("inflateInit2 failed");
        break;
      }

      std::vector<unsigned char> output(input.size() + 1024);
      dstream.next_in = compressed.data();
      dstream.avail_in = static_cast<unsigned int>(compressed_size);
      dstream.next_out = output.data();
      dstream.avail_out = static_cast<unsigned int>(output.size());

      ret = inflate(&dstream, Z_NO_FLUSH);
      if (ret == Z_NEED_DICT) {
        ret = inflateSetDictionary(
            &dstream, reinterpret_cast<const Bytef*>(dictionary.data()),
            static_cast<uInt>(dictionary.size()));
        if (ret == Z_OK) {
          ret = inflate(&dstream, Z_FINISH);
        }
      }

      const size_t produced = output.size() - dstream.avail_out;
      const bool output_matches =
          (produced == input.size()) &&
          (memcmp(output.data(), input.data(), input.size()) == 0);

      inflateEnd(&dstream);

      if (ret != Z_STREAM_END) {
        set_failure("inflate did not reach Z_STREAM_END");
        break;
      }
      if (!output_matches) {
        set_failure("round-trip data mismatch");
        break;
      }
    }

    deflateEnd(&cstream);
  };

  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (int worker_id = 0; worker_id < thread_count; ++worker_id) {
    workers.emplace_back(worker, worker_id);
  }
  for (auto& worker_thread : workers) {
    worker_thread.join();
  }

  ASSERT_FALSE(failed.load()) << failure_reason;
}

TEST(IGZIPDeflateRegressionTest,
     IGZIPDictionaryOutputMustBeZlibDictionaryCompatible) {
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(ZLIB, false, false);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  const std::string dictionary =
      "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string input;
  for (int i = 0; i < 512; ++i) {
    input += "doc:";
    input += dictionary;
    input += "|";
  }

  z_stream cstream;
  memset(&cstream, 0, sizeof(cstream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  ASSERT_EQ(deflateSetDictionary(
                &cstream, reinterpret_cast<const Bytef*>(dictionary.data()),
                static_cast<uInt>(dictionary.size())),
            Z_OK);
  ASSERT_EQ(GetDeflateExecutionPath(&cstream), ZLIB);

  std::vector<unsigned char> compressed(compressBound(input.size()));
  cstream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  cstream.avail_in = static_cast<unsigned int>(input.size());
  cstream.next_out = compressed.data();
  cstream.avail_out = static_cast<unsigned int>(compressed.size());

  int ret = Z_OK;
  for (int iter = 0; iter < 32; ++iter) {
    ret = deflate(&cstream, Z_FINISH);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  const size_t compressed_size = compressed.size() - cstream.avail_out;
  deflateEnd(&cstream);

  z_stream dstream;
  memset(&dstream, 0, sizeof(dstream));
  ASSERT_EQ(inflateInit2(&dstream, 15), Z_OK);

  std::vector<unsigned char> uncompressed(input.size() + 1024);
  dstream.next_in = compressed.data();
  dstream.avail_in = static_cast<unsigned int>(compressed_size);
  dstream.next_out = uncompressed.data();
  dstream.avail_out = static_cast<unsigned int>(uncompressed.size());

  ret = inflate(&dstream, Z_NO_FLUSH);
  if (ret == Z_NEED_DICT) {
    ASSERT_EQ(inflateSetDictionary(
                  &dstream, reinterpret_cast<const Bytef*>(dictionary.data()),
                  static_cast<uInt>(dictionary.size())),
              Z_OK);
    ret = inflate(&dstream, Z_FINISH);
  }

  EXPECT_EQ(ret, Z_STREAM_END);
  const size_t produced = uncompressed.size() - dstream.avail_out;
  ASSERT_EQ(produced, input.size());
  EXPECT_EQ(memcmp(uncompressed.data(), input.data(), input.size()), 0);

  inflateEnd(&dstream);
}

TEST(IGZIPDeflateRegressionTest,
     FinishWithTinyOutputBufferMustNotTruncateStream) {
  SetCompressPath(IGZIP, true, false, false);
  SetUncompressPath(ZLIB, false, false);
  if (GetConfig(USE_ZLIB_COMPRESS) == 0) {
    GTEST_SKIP() << "USE_ZLIB_COMPRESS=0 disables fallback-first contract";
  }

  std::string input;
  input.reserve(256 * 1024);
  for (int i = 0; i < 4096; ++i) {
    input += "{\"k\":";
    input += std::to_string(i);
    input += ",\"msg\":\"abcdefghijklmnopqrstuvwxyz\"}";
  }

  z_stream cstream;
  memset(&cstream, 0, sizeof(cstream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  cstream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  cstream.avail_in = static_cast<unsigned int>(input.size());

  std::vector<unsigned char> compressed;
  compressed.reserve(input.size());

  int ret = Z_OK;
  for (int iter = 0; iter < 20000; ++iter) {
    unsigned char out_chunk[64];
    cstream.next_out = out_chunk;
    cstream.avail_out = sizeof(out_chunk);

    ret = deflate(&cstream, Z_FINISH);
    const ExecutionPath path = GetDeflateExecutionPath(&cstream);
    ASSERT_TRUE(path == IGZIP || path == ZLIB);
    ASSERT_NE(ret, Z_DATA_ERROR) << "iter=" << iter;

    const size_t produced = sizeof(out_chunk) - cstream.avail_out;
    compressed.insert(compressed.end(), out_chunk, out_chunk + produced);

    if (ret == Z_STREAM_END) {
      break;
    }
  }

  ASSERT_EQ(ret, Z_STREAM_END);
  deflateEnd(&cstream);

  z_stream dstream;
  memset(&dstream, 0, sizeof(dstream));
  ASSERT_EQ(inflateInit2(&dstream, 15), Z_OK);

  std::vector<unsigned char> output(input.size() + 1024);
  dstream.next_in = compressed.data();
  dstream.avail_in = static_cast<unsigned int>(compressed.size());
  dstream.next_out = output.data();
  dstream.avail_out = static_cast<unsigned int>(output.size());

  for (int iter = 0; iter < 1024; ++iter) {
    ret = inflate(&dstream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
    if (ret == Z_BUF_ERROR && dstream.avail_in == 0) {
      break;
    }
  }

  ASSERT_EQ(ret, Z_STREAM_END);
  const size_t out_size = output.size() - dstream.avail_out;
  ASSERT_EQ(out_size, input.size());
  EXPECT_EQ(memcmp(output.data(), input.data(), input.size()), 0);

  inflateEnd(&dstream);
}

TEST(IGZIPDeflateRegressionTest, SyncFlushWithInputMustStayOnIGZIPPath) {
  // Originally this test asserted the path was ZLIB (IGZIPShouldFallbackDeflate
  // redirected SYNC_FLUSH to zlib). That function is removed; IGZIP now handles
  // SYNC_FLUSH natively and the stream must stay on IGZIP throughout.
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(ZLIB, false, false);

  std::string input;
  input.reserve(64 * 1024);
  for (int i = 0; i < 1024; ++i) {
    input += "record-";
    input += std::to_string(i);
    input += "-abcdefghijklmnopqrstuvwxyz";
  }

  z_stream cstream;
  memset(&cstream, 0, sizeof(cstream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<unsigned char> compressed;
  compressed.reserve(input.size());

  cstream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  cstream.avail_in = static_cast<unsigned int>(input.size());

  unsigned char sync_chunk[256];
  cstream.next_out = sync_chunk;
  cstream.avail_out = sizeof(sync_chunk);

  int ret = deflate(&cstream, Z_SYNC_FLUSH);
  ASSERT_NE(ret, Z_DATA_ERROR);
  ASSERT_EQ(GetDeflateExecutionPath(&cstream), IGZIP);
  const size_t sync_produced = sizeof(sync_chunk) - cstream.avail_out;
  compressed.insert(compressed.end(), sync_chunk, sync_chunk + sync_produced);

  for (int iter = 0; iter < 8192; ++iter) {
    unsigned char out_chunk[256];
    cstream.next_out = out_chunk;
    cstream.avail_out = sizeof(out_chunk);
    cstream.next_in = nullptr;
    cstream.avail_in = 0;

    ret = deflate(&cstream, Z_FINISH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    ASSERT_EQ(GetDeflateExecutionPath(&cstream), IGZIP);

    const size_t produced = sizeof(out_chunk) - cstream.avail_out;
    compressed.insert(compressed.end(), out_chunk, out_chunk + produced);

    if (ret == Z_STREAM_END) {
      break;
    }
  }

  ASSERT_EQ(ret, Z_STREAM_END);
  deflateEnd(&cstream);

  z_stream dstream;
  memset(&dstream, 0, sizeof(dstream));
  ASSERT_EQ(inflateInit2(&dstream, 15), Z_OK);

  std::vector<unsigned char> output(input.size() + 1024);
  dstream.next_in = compressed.data();
  dstream.avail_in = static_cast<unsigned int>(compressed.size());
  dstream.next_out = output.data();
  dstream.avail_out = static_cast<unsigned int>(output.size());

  for (int iter = 0; iter < 1024; ++iter) {
    ret = inflate(&dstream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  ASSERT_EQ(ret, Z_STREAM_END);
  const size_t out_size = output.size() - dstream.avail_out;
  ASSERT_EQ(out_size, input.size());
  EXPECT_EQ(memcmp(output.data(), input.data(), input.size()), 0);

  inflateEnd(&dstream);
}

TEST(IGZIPDeflateRegressionTest, Compress2MustDetectTruncatedOutput) {
  // Regression: compress2() returned Z_OK on a truncated stream when destLen
  // was too small to hold the ISA-L trailer.  ISA-L consumes all input
  // (COMP_OK) but leaves the stream in a non-terminal state; the old
  // input_len != sourceLen check missed this case.
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);

  const std::vector<unsigned char> source(64, 'A');

  // Determine the true compressed size with an adequately sized buffer.
  uLongf correct_destLen = compressBound(source.size());
  std::vector<unsigned char> correct_dest(correct_destLen);
  ASSERT_EQ(compress2(correct_dest.data(), &correct_destLen, source.data(),
                      source.size(), Z_DEFAULT_COMPRESSION),
            Z_OK);

  // Call again with a buffer one byte too small for the complete output.
  uLongf small_destLen = correct_destLen - 1;
  std::vector<unsigned char> small_dest(small_destLen);
  int ret = compress2(small_dest.data(), &small_destLen, source.data(),
                      source.size(), Z_DEFAULT_COMPRESSION);
  // Must NOT silently succeed on truncated output.
  EXPECT_NE(ret, Z_OK);
}

TEST(IGZIPInflateRegressionTest,
     NeedDictFromIGZIPMustFallbackToZlibOnFirstInflateCall) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  const std::string dictionary =
      "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string input;
  for (int i = 0; i < 512; ++i) {
    input += "doc:";
    input += dictionary;
    input += "|";
  }

  z_stream cstream;
  memset(&cstream, 0, sizeof(cstream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  ASSERT_EQ(deflateSetDictionary(
                &cstream, reinterpret_cast<const Bytef*>(dictionary.data()),
                static_cast<uInt>(dictionary.size())),
            Z_OK);
  ASSERT_EQ(GetDeflateExecutionPath(&cstream), ZLIB);

  std::vector<unsigned char> compressed(compressBound(input.size()));
  cstream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  cstream.avail_in = static_cast<unsigned int>(input.size());
  cstream.next_out = compressed.data();
  cstream.avail_out = static_cast<unsigned int>(compressed.size());

  int ret = Z_OK;
  for (int iter = 0; iter < 64; ++iter) {
    ret = deflate(&cstream, Z_FINISH);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  const size_t compressed_size = compressed.size() - cstream.avail_out;
  deflateEnd(&cstream);

  z_stream dstream;
  memset(&dstream, 0, sizeof(dstream));
  ASSERT_EQ(inflateInit2(&dstream, 15), Z_OK);

  std::vector<unsigned char> uncompressed(input.size() + 1024);
  dstream.next_in = compressed.data();
  dstream.avail_in = static_cast<unsigned int>(compressed_size);
  dstream.next_out = uncompressed.data();
  dstream.avail_out = static_cast<unsigned int>(uncompressed.size());

  ret = inflate(&dstream, Z_NO_FLUSH);
  ASSERT_EQ(GetInflateExecutionPath(&dstream), ZLIB);
  ASSERT_EQ(ret, Z_NEED_DICT);

  ASSERT_EQ(inflateSetDictionary(
                &dstream, reinterpret_cast<const Bytef*>(dictionary.data()),
                static_cast<uInt>(dictionary.size())),
            Z_OK);
  ret = inflate(&dstream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);

  const size_t out_size = uncompressed.size() - dstream.avail_out;
  ASSERT_EQ(out_size, input.size());
  EXPECT_EQ(memcmp(uncompressed.data(), input.data(), input.size()), 0);

  inflateEnd(&dstream);
}

// windowBits 16..31 requests gzip-only decoding. 16 used to fall through to
// the IGZIP_ZLIB branch of ConfigureInflateWindow, so IGZIP decoded
// zlib-format data that zlib itself rejects with Z_DATA_ERROR -- the shim was
// more permissive than the library it replaces.
TEST(IGZIPInflateRegressionTest, GzipOnlyWindowBitsMustRejectZlibData) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  const size_t input_length = 16384;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  // zlib-format (not gzip) stream.
  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath execution_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &execution_path),
            Z_STREAM_END);
  ASSERT_EQ(execution_path, ZLIB);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 16), Z_OK);

  std::vector<char> output(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  stream.avail_in = static_cast<unsigned int>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<unsigned int>(output.size());

  // Gzip-only was requested, so a zlib header must be an error, not output.
  EXPECT_EQ(inflate(&stream, Z_FINISH), Z_DATA_ERROR);
  EXPECT_EQ(stream.total_out, 0u);

  inflateEnd(&stream);
  DestroyBlock(input);
}

// windowBits >= 32 asks for automatic zlib/gzip header detection, which ISA-L
// cannot express. SupportedOptionsIGZIPInflate() keeps the range off the IGZIP
// path so zlib handles it; both wrapper formats must still decode.
TEST(IGZIPInflateRegressionTest, AutoDetectWindowBitsDecodesBothFormats) {
  SetCompressPath(ZLIB, false, false, false);
  // zlib_fallback must be on: the range is gated off IGZIP, so zlib has to be
  // available to take the stream.
  SetUncompressPath(IGZIP, /*zlib_fallback=*/true, false);

  const size_t input_length = 16384;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  // 15 == zlib wrapper, 31 == gzip wrapper. Auto-detect must accept both.
  for (const int compress_window_bits : {15, 31}) {
    std::string compressed;
    size_t output_upper_bound;
    ExecutionPath execution_path = UNDEFINED;
    ASSERT_EQ(
        ZlibCompress(input, input_length, &compressed, compress_window_bits,
                     Z_FINISH, &output_upper_bound, &execution_path),
        Z_STREAM_END)
        << "compress window_bits " << compress_window_bits;
    ASSERT_EQ(execution_path, ZLIB);

    z_stream stream;
    memset(&stream, 0, sizeof(z_stream));
    ASSERT_EQ(inflateInit2(&stream, 47), Z_OK);

    std::vector<char> output(input_length + 1024);
    stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<unsigned int>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());

    EXPECT_EQ(inflate(&stream, Z_FINISH), Z_STREAM_END)
        << "compress window_bits " << compress_window_bits;
    EXPECT_EQ(stream.total_out, input_length);
    EXPECT_EQ(memcmp(output.data(), input, input_length), 0);
    // Auto-detect is gated off IGZIP; zlib owns the stream.
    EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);

    inflateEnd(&stream);
  }

  DestroyBlock(input);
}

// Returns true if the buffer contains a 00 00 FF FF deflate sync marker.
bool ContainsSyncMarker(const char* data, size_t length) {
  for (size_t i = 0; i + 3 < length; i++) {
    if (static_cast<unsigned char>(data[i]) == 0x00 &&
        static_cast<unsigned char>(data[i + 1]) == 0x00 &&
        static_cast<unsigned char>(data[i + 2]) == 0xff &&
        static_cast<unsigned char>(data[i + 3]) == 0xff) {
      return true;
    }
  }
  return false;
}

// Z_BLOCK ends a deflate block without byte-aligning and without emitting the
// 00 00 FF FF sync marker. ISA-L's only flushing modes (SYNC_FLUSH,
// FULL_FLUSH) always do both, so SupportedOptionsIGZIPDeflate() keeps a
// Z_BLOCK stream off the IGZIP path and zlib produces the framing.
TEST(IGZIPDeflateRegressionTest, ZBlockStaysOnZlibAndEmitsNoSyncMarker) {
  // zlib_fallback must be on: the flush value is gated off IGZIP, so zlib has
  // to be available to take the stream.
  SetCompressPath(IGZIP, /*zlib_fallback=*/true, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length));
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  EXPECT_EQ(deflate(&stream, Z_BLOCK), Z_OK);
  const size_t produced = output.size() - stream.avail_out;
  EXPECT_GT(produced, 0u);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);
  EXPECT_FALSE(ContainsSyncMarker(reinterpret_cast<const char*>(output.data()),
                                  produced))
      << "Z_BLOCK must not emit a 00 00 FF FF sync marker";

  deflateEnd(&stream);
  DestroyBlock(input);
}

// The predicate only gates path *selection*. A stream that lands on IGZIP
// under an allowed flush value and then switches to Z_BLOCK mid-stream stays
// on IGZIP by stickiness -- ISA-L holds unflushed state, so it cannot be
// migrated to zlib without corrupting the output. Z_BLOCK is then treated as
// Z_SYNC_FLUSH: an extra sync marker, but still valid deflate that
// round-trips. This documents the accepted residual behavior.
TEST(IGZIPDeflateRegressionTest, ZBlockMidStreamStaysOnIGZIPAndRoundTrips) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> buffer(deflateBound(&stream, input_length) + 4096);
  std::string compressed;

  // Land the stream on IGZIP with a flush value the predicate allows.
  const size_t first_half = input_length / 2;
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(first_half);
  stream.next_out = buffer.data();
  stream.avail_out = static_cast<uInt>(buffer.size());
  ASSERT_EQ(deflate(&stream, Z_SYNC_FLUSH), Z_OK);
  ASSERT_EQ(GetDeflateExecutionPath(&stream), IGZIP);
  compressed.append(reinterpret_cast<const char*>(buffer.data()),
                    buffer.size() - stream.avail_out);

  // Now switch to Z_BLOCK mid-stream: stickiness keeps it on IGZIP.
  stream.next_in = reinterpret_cast<Bytef*>(input) + first_half;
  stream.avail_in = static_cast<uInt>(input_length - first_half);
  stream.next_out = buffer.data();
  stream.avail_out = static_cast<uInt>(buffer.size());
  EXPECT_EQ(deflate(&stream, Z_BLOCK), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), IGZIP);
  compressed.append(reinterpret_cast<const char*>(buffer.data()),
                    buffer.size() - stream.avail_out);

  // Finish the stream.
  int ret = Z_OK;
  for (int i = 0; i < 64 && ret != Z_STREAM_END; i++) {
    stream.next_out = buffer.data();
    stream.avail_out = static_cast<uInt>(buffer.size());
    ret = deflate(&stream, Z_FINISH);
    ASSERT_GE(ret, Z_OK);
    compressed.append(reinterpret_cast<const char*>(buffer.data()),
                      buffer.size() - stream.avail_out);
  }
  EXPECT_EQ(ret, Z_STREAM_END);
  deflateEnd(&stream);

  // The extra sync marker is harmless: the stream must still decode exactly.
  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(ZlibUncompress(compressed.data(), compressed.size(), input_length,
                           &uncompressed, &uncompressed_length, &input_consumed,
                           15, Z_FINISH, 1, &uncompress_path),
            Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);

  DestroyBlock(uncompressed);
  DestroyBlock(input);
}

// No backend honors deflateInit2's strategy argument (zlib defines it as
// affecting compression ratio, not correctness). Output must still be valid
// and round-trip exactly. Compressed size is deliberately not asserted -- it
// is an ISA-L implementation detail that changes between versions.
TEST(IGZIPDeflateRegressionTest, NonDefaultStrategiesStillRoundTrip) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  for (const int strategy :
       {Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED}) {
    z_stream stream;
    memset(&stream, 0, sizeof(z_stream));
    ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                           strategy),
              Z_OK)
        << "strategy " << strategy;

    std::vector<Bytef> output(deflateBound(&stream, input_length) + 4096);
    stream.next_in = reinterpret_cast<Bytef*>(input);
    stream.avail_in = static_cast<uInt>(input_length);
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());

    ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END)
        << "strategy " << strategy;
    const size_t produced = output.size() - stream.avail_out;
    EXPECT_EQ(GetDeflateExecutionPath(&stream), IGZIP)
        << "strategy " << strategy << " must not change path selection";
    deflateEnd(&stream);

    char* uncompressed = nullptr;
    size_t uncompressed_length = 0;
    size_t input_consumed = 0;
    ExecutionPath uncompress_path = UNDEFINED;
    ASSERT_EQ(
        ZlibUncompress(reinterpret_cast<const char*>(output.data()), produced,
                       input_length, &uncompressed, &uncompressed_length,
                       &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
        Z_STREAM_END)
        << "strategy " << strategy;
    EXPECT_EQ(uncompressed_length, input_length) << "strategy " << strategy;
    EXPECT_EQ(memcmp(uncompressed, input, input_length), 0)
        << "strategy " << strategy;
    DestroyBlock(uncompressed);
  }

  DestroyBlock(input);
}

// Z_NO_COMPRESSION (level 0) asks for stored, uncompressed deflate blocks,
// which no backend can produce.  Such a stream must be pinned to zlib at path
// selection: before the fix InitCompressIGZIP() rejected the level per call and
// deflate() returned Z_DATA_ERROR forever whenever use_zlib_compress was 0,
// because deflate_settings->path was still UNDEFINED at the zlib fall-through.
TEST(IGZIPDeflateRegressionTest, NoCompressionLevelStaysOnZlibAndStoresInput) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_NO_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - stream.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);
  deflateEnd(&stream);

  // Stored blocks are slightly larger than the input (5 bytes of block header
  // per 65535, plus wrapper).  Any real compression of a compressible block
  // would be far smaller, so this is what proves level 0 was actually honored
  // rather than silently compressed by a backend.
  EXPECT_GT(produced, input_length);
  EXPECT_LT(produced, input_length + 1024);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(
      ZlibUncompress(reinterpret_cast<const char*>(output.data()), produced,
                     input_length, &uncompressed, &uncompressed_length,
                     &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
      Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);
  DestroyBlock(uncompressed);

  DestroyBlock(input);
}

// Same contract on the one-shot path.  compress2() has no per-stream path to
// pin, so it carries the decision in a local and must reach zlib even with
// use_zlib_compress == 0 -- the request was never an offload candidate.
TEST(IGZIPDeflateRegressionTest, Compress2NoCompressionLevelStoresInput) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  uLong destLen = compressBound(static_cast<uLong>(input_length));
  std::vector<Bytef> dest(destLen);

  ASSERT_EQ(
      compress2(dest.data(), &destLen, reinterpret_cast<const Bytef*>(input),
                static_cast<uLong>(input_length), Z_NO_COMPRESSION),
      Z_OK);
  EXPECT_GT(destLen, static_cast<uLong>(input_length));

  std::vector<Bytef> round_trip(input_length);
  uLong round_trip_len = static_cast<uLong>(round_trip.size());
  ASSERT_EQ(
      uncompress(round_trip.data(), &round_trip_len, dest.data(), destLen),
      Z_OK);
  EXPECT_EQ(round_trip_len, static_cast<uLong>(input_length));
  EXPECT_EQ(memcmp(round_trip.data(), input, input_length), 0);

  DestroyBlock(input);
}

// An application may call inflate() with avail_in == 0 and next_in == nullptr
// on an active IGZIP stream. next_in is never dereferenced at length 0, but it
// must not be handed to ISA-L as NULL either.
TEST(IGZIPInflateRegressionTest, ActiveStreamHandlesNullNextInWithZeroAvailIn) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 16384;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<char> output(input_length + 1024);

  // First call: feed only part of the input so the IGZIP stream stays active.
  const size_t partial = compressed.size() / 2;
  stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  stream.avail_in = static_cast<unsigned int>(partial);
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<unsigned int>(output.size());
  ASSERT_GE(inflate(&stream, Z_NO_FLUSH), Z_OK);
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);

  // Second call: no input at all, and next_in explicitly NULL.
  stream.next_in = nullptr;
  stream.avail_in = 0;
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<unsigned int>(output.size());
  const int ret = inflate(&stream, Z_NO_FLUSH);
  EXPECT_TRUE(ret == Z_BUF_ERROR || ret == Z_OK)
      << "expected Z_BUF_ERROR or Z_OK, got " << ret;

  inflateEnd(&stream);
  DestroyBlock(input);
}

#ifdef USE_IAA
// IAA->IGZIP fallback tests.
// On machines without IAA hardware, CompressIAA/UncompressIAA return non-zero,
// which naturally triggers the fallback path. These tests verify:
//   - When igzip_fallback=1: the stream lands on IGZIP after IAA fails.
//   - When igzip_fallback=0: the stream falls through to zlib, not IGZIP.

TEST(IAAFallbackIGZIPTest, DeflateUsesIGZIPWhenIAAFailsAndFallbackEnabled) {
  SetCompressPath(IAA, /*zlib_fallback=*/true,
                  /*iaa_prepend_empty_block=*/false,
                  /*qat_compression_allow_chunking=*/false);
  SetConfig(USE_IGZIP_COMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 1);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length));
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  int ret = deflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);

  // If IAA hardware is absent, the fallback must have routed to IGZIP.
  // If IAA hardware is present and succeeds, IAA path is also acceptable.
  const ExecutionPath path = GetDeflateExecutionPath(&stream);
  EXPECT_TRUE(path == IGZIP || path == IAA)
      << "Expected IGZIP (fallback) or IAA (hardware success), got "
      << static_cast<int>(path);

  deflateEnd(&stream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}

TEST(IAAFallbackIGZIPTest, DeflateDoesNotUseIGZIPWhenFallbackDisabled) {
  SetCompressPath(IAA, /*zlib_fallback=*/true,
                  /*iaa_prepend_empty_block=*/false,
                  /*qat_compression_allow_chunking=*/false);
  SetConfig(USE_IGZIP_COMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 0);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length));
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  const int ret = deflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);

  // With fallback disabled, IGZIP must not be selected for an IAA-configured
  // stream; it should fall through to zlib.
  const ExecutionPath path = GetDeflateExecutionPath(&stream);
  EXPECT_NE(path, IGZIP) << "IGZIP must not be used when igzip_fallback=0";

  deflateEnd(&stream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}

// A level-0 stream is pinned to zlib before any backend is consulted, so the
// accelerator->IGZIP fallback block must be unreachable for it even with
// igzip_fallback=1.  Hardware-independent: IAA is never attempted either way,
// so the expected path is strictly ZLIB on any machine.
TEST(IAAFallbackIGZIPTest, NoCompressionLevelMustNotFallBackToIGZIP) {
  SetCompressPath(IAA, /*zlib_fallback=*/false,
                  /*iaa_prepend_empty_block=*/false,
                  /*qat_compression_allow_chunking=*/false);
  SetConfig(USE_IGZIP_COMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 1);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_NO_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - stream.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);
  EXPECT_GT(produced, input_length);

  deflateEnd(&stream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}

TEST(IAAFallbackIGZIPTest, InflateUsesIGZIPWhenIAAFailsAndFallbackEnabled) {
  // Compress with IGZIP so the output is IGZIP-compatible (4kB window).
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(IAA, /*zlib_fallback=*/true,
                    /*iaa_prepend_empty_block=*/false);
  SetConfig(USE_IGZIP_UNCOMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 1);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath compress_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &compress_path);
  ASSERT_EQ(ret, Z_STREAM_END);

  z_stream dstream;
  memset(&dstream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&dstream, -15), Z_OK);

  std::vector<char> uncompressed(input_length);
  dstream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  dstream.avail_in = static_cast<uInt>(compressed.size());
  dstream.next_out = reinterpret_cast<Bytef*>(uncompressed.data());
  dstream.avail_out = static_cast<uInt>(uncompressed.size());

  ret = inflate(&dstream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);

  // If IAA hardware is absent, fallback must route to IGZIP.
  // If IAA hardware is present and succeeds, IAA is also acceptable.
  const ExecutionPath path = GetInflateExecutionPath(&dstream);
  EXPECT_TRUE(path == IGZIP || path == IAA)
      << "Expected IGZIP (fallback) or IAA (hardware success), got "
      << static_cast<int>(path);

  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);

  inflateEnd(&dstream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}

TEST(IAAFallbackIGZIPTest, InflateDoesNotUseIGZIPWhenFallbackDisabled) {
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(IAA, /*zlib_fallback=*/true,
                    /*iaa_prepend_empty_block=*/false);
  SetConfig(USE_IGZIP_UNCOMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 0);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath compress_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &compress_path);
  ASSERT_EQ(ret, Z_STREAM_END);

  z_stream dstream;
  memset(&dstream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&dstream, -15), Z_OK);

  std::vector<char> uncompressed(input_length);
  dstream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  dstream.avail_in = static_cast<uInt>(compressed.size());
  dstream.next_out = reinterpret_cast<Bytef*>(uncompressed.data());
  dstream.avail_out = static_cast<uInt>(uncompressed.size());

  inflate(&dstream, Z_FINISH);

  // With fallback disabled, IGZIP must not be selected for an IAA-configured
  // inflate stream; it should fall through to zlib.
  const ExecutionPath path = GetInflateExecutionPath(&dstream);
  EXPECT_NE(path, IGZIP) << "IGZIP must not be used when igzip_fallback=0";

  inflateEnd(&dstream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}
#endif  // USE_IAA

#if defined(USE_QAT) && defined(USE_IGZIP)
// QAT->IGZIP fallback tests.
// On machines without QAT hardware, CompressQAT/UncompressQAT return non-zero,
// which naturally triggers the fallback path. These tests verify:
//   - When igzip_fallback=1: the stream lands on IGZIP after QAT fails.
//   - When igzip_fallback=0: the stream falls through to zlib, not IGZIP.

TEST(QATFallbackIGZIPTest, DeflateUsesIGZIPWhenQATFailsAndFallbackEnabled) {
  SetCompressPath(QAT, /*zlib_fallback=*/true,
                  /*iaa_prepend_empty_block=*/false,
                  /*qat_compression_allow_chunking=*/false);
  SetConfig(USE_IGZIP_COMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 1);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length));
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  int ret = deflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);

  // If QAT hardware is absent, fallback must route to IGZIP.
  // If QAT hardware is present and succeeds, QAT path is also acceptable.
  const ExecutionPath path = GetDeflateExecutionPath(&stream);
  EXPECT_TRUE(path == IGZIP || path == QAT)
      << "Expected IGZIP (fallback) or QAT (hardware success), got "
      << static_cast<int>(path);

  deflateEnd(&stream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}

TEST(QATFallbackIGZIPTest, DeflateDoesNotUseIGZIPWhenFallbackDisabled) {
  SetCompressPath(QAT, /*zlib_fallback=*/true,
                  /*iaa_prepend_empty_block=*/false,
                  /*qat_compression_allow_chunking=*/false);
  SetConfig(USE_IGZIP_COMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 0);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length));
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  int ret = deflate(&stream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);

  // With fallback disabled, IGZIP must not be selected for a QAT-configured
  // stream; it should fall through to zlib.
  const ExecutionPath path = GetDeflateExecutionPath(&stream);
  EXPECT_NE(path, IGZIP) << "IGZIP must not be used when igzip_fallback=0";

  deflateEnd(&stream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}

TEST(QATFallbackIGZIPTest, InflateUsesIGZIPWhenQATFailsAndFallbackEnabled) {
  // Compress with IGZIP so the output is IGZIP-compatible (4kB window).
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(QAT, /*zlib_fallback=*/true,
                    /*iaa_prepend_empty_block=*/false);
  SetConfig(USE_IGZIP_UNCOMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 1);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath compress_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &compress_path);
  ASSERT_EQ(ret, Z_STREAM_END);

  z_stream dstream;
  memset(&dstream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&dstream, -15), Z_OK);

  std::vector<char> uncompressed(input_length);
  dstream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  dstream.avail_in = static_cast<uInt>(compressed.size());
  dstream.next_out = reinterpret_cast<Bytef*>(uncompressed.data());
  dstream.avail_out = static_cast<uInt>(uncompressed.size());

  ret = inflate(&dstream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);

  // If QAT hardware is absent, fallback must route to IGZIP.
  // If QAT hardware is present and succeeds, QAT is also acceptable.
  const ExecutionPath path = GetInflateExecutionPath(&dstream);
  EXPECT_TRUE(path == IGZIP || path == QAT)
      << "Expected IGZIP (fallback) or QAT (hardware success), got "
      << static_cast<int>(path);

  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);

  inflateEnd(&dstream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}

TEST(QATFallbackIGZIPTest, InflateDoesNotUseIGZIPWhenFallbackDisabled) {
  SetCompressPath(IGZIP, false, false, false);
  SetUncompressPath(QAT, /*zlib_fallback=*/true,
                    /*iaa_prepend_empty_block=*/false);
  SetConfig(USE_IGZIP_UNCOMPRESS, 1);
  SetConfig(IGZIP_FALLBACK, 0);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath compress_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &compress_path);
  ASSERT_EQ(ret, Z_STREAM_END);

  z_stream dstream;
  memset(&dstream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&dstream, -15), Z_OK);

  std::vector<char> uncompressed(input_length);
  dstream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  dstream.avail_in = static_cast<uInt>(compressed.size());
  dstream.next_out = reinterpret_cast<Bytef*>(uncompressed.data());
  dstream.avail_out = static_cast<uInt>(uncompressed.size());

  ret = inflate(&dstream, Z_FINISH);
  ASSERT_EQ(ret, Z_STREAM_END);

  // With fallback disabled, IGZIP must not be selected for a QAT-configured
  // inflate stream; it should fall through to zlib.
  const ExecutionPath path = GetInflateExecutionPath(&dstream);
  EXPECT_NE(path, IGZIP) << "IGZIP must not be used when igzip_fallback=0";

  inflateEnd(&dstream);
  SetConfig(IGZIP_FALLBACK, 0);
  DestroyBlock(input);
}
#endif  // USE_QAT && USE_IGZIP

#endif  // USE_IGZIP

// ---------------------------------------------------------------------------
// inflate() flush gate: Z_BLOCK / Z_TREES
//
// Both flush values ask inflate() to stop at a deflate block boundary (Z_TREES
// also at the end of each block header) and to report the bit position reached
// in data_type. No backend can do either, so such a stream is pinned to zlib.
// These tests are all-backend by design -- the gate is in zlib_accel.cpp, not
// in a backend wrapper.
// ---------------------------------------------------------------------------

namespace {

// One inflate() return, recorded so a shimmed run can be compared to a
// plain-zlib control run of the same bytes. Block boundaries are a zlib
// implementation detail, so nothing here is hardcoded.
struct InflateStep {
  int ret;
  unsigned long total_in;
  unsigned long total_out;
  int data_type;
};

// Builds a raw-deflate buffer holding several blocks. Z_FULL_FLUSH between
// thirds guarantees more than one block boundary exists, which is what makes an
// early Z_BLOCK return observable at all. Raw (windowBits -15) is deliberate:
// with a zlib/gzip wrapper, Z_BLOCK's first return comes after the header
// rather than after a block, which is a weaker signal.
std::string BuildMultiBlockRawDeflate(const char* input, size_t input_length) {
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);

  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  // Bail out rather than EXPECT_EQ and continue: deflateBound() and deflate()
  // below would touch an uninitialized stream. ASSERT_EQ cannot be used here
  // because it expands to a bare `return;` and this helper returns a value; the
  // empty string is caught by the caller's ASSERT_GT on the result size.
  if (deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) !=
      Z_OK) {
    ADD_FAILURE() << "deflateInit2 failed";
    return std::string();
  }

  std::vector<Bytef> buffer(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input));
  stream.next_out = buffer.data();
  stream.avail_out = static_cast<uInt>(buffer.size());

  const size_t third = input_length / 3;
  stream.avail_in = static_cast<uInt>(third);
  EXPECT_EQ(deflate(&stream, Z_FULL_FLUSH), Z_OK);
  stream.avail_in = static_cast<uInt>(third);
  EXPECT_EQ(deflate(&stream, Z_FULL_FLUSH), Z_OK);

  stream.avail_in = static_cast<uInt>(input_length - 2 * third);
  int ret = Z_OK;
  for (int i = 0; i < 64 && ret != Z_STREAM_END; i++) {
    ret = deflate(&stream, Z_FINISH);
    EXPECT_GE(ret, Z_OK);
  }
  EXPECT_EQ(ret, Z_STREAM_END);

  const size_t produced = buffer.size() - stream.avail_out;
  deflateEnd(&stream);
  return std::string(reinterpret_cast<const char*>(buffer.data()), produced);
}

// Drives a full raw inflate with one flush value, recording every return.
// Z_BUF_ERROR is not fatal here: with Z_TREES zlib legitimately returns it at a
// boundary where no progress was possible, and the caller simply calls again.
std::vector<InflateStep> RunInflateSteps(const std::string& compressed,
                                         int flush, size_t expected_length,
                                         std::string* output,
                                         ExecutionPath* path_after_first) {
  std::vector<InflateStep> steps;
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  // See BuildMultiBlockRawDeflate: bail out rather than run inflate() on an
  // uninitialized stream. No steps recorded, which every caller detects -- via
  // ASSERT_FALSE(steps.empty()), the control-size check, or a size mismatch
  // against the control run.
  if (inflateInit2(&stream, -15) != Z_OK) {
    ADD_FAILURE() << "inflateInit2 failed";
    return steps;
  }

  std::vector<Bytef> buffer(expected_length + 4096);
  stream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = buffer.data();
  stream.avail_out = static_cast<uInt>(buffer.size());

  for (int i = 0; i < 64; i++) {
    const int ret = inflate(&stream, flush);
    steps.push_back({ret, stream.total_in, stream.total_out, stream.data_type});
    if (i == 0 && path_after_first != nullptr) {
      *path_after_first = GetInflateExecutionPath(&stream);
    }
    if (ret == Z_STREAM_END) {
      break;
    }
    if (ret < 0 && ret != Z_BUF_ERROR) {
      break;
    }
    if (ret == Z_BUF_ERROR && stream.avail_in == 0) {
      break;
    }
  }

  if (output != nullptr) {
    output->assign(reinterpret_cast<const char*>(buffer.data()),
                   stream.total_out);
  }
  inflateEnd(&stream);
  return steps;
}

// The accelerator paths this build was configured with. ZLIB is excluded: it is
// the control, not a case under test.
std::vector<ExecutionPath> ConfiguredAcceleratorPaths() {
  std::vector<ExecutionPath> paths;
#ifdef USE_QAT
  paths.push_back(QAT);
#endif
#ifdef USE_IAA
  paths.push_back(IAA);
#endif
#ifdef USE_IGZIP
  paths.push_back(IGZIP);
#endif
  return paths;
}

std::string PathLabel(ExecutionPath path) {
  switch (path) {
    case QAT:
      return "QAT";
    case IAA:
      return "IAA";
    case IGZIP:
      return "IGZIP";
    case ZLIB:
      return "ZLIB";
    default:
      return "UNDEFINED";
  }
}

constexpr size_t kFlushGateInputLength = 96 * 1024;

}  // namespace

class InflateFlushGateTest : public testing::Test {
 protected:
  void SetUp() override {
    saved_use_zlib_uncompress_ = GetConfig(USE_ZLIB_UNCOMPRESS);
    saved_use_iaa_uncompress_ = GetConfig(USE_IAA_UNCOMPRESS);
    saved_use_qat_uncompress_ = GetConfig(USE_QAT_UNCOMPRESS);
    saved_use_igzip_uncompress_ = GetConfig(USE_IGZIP_UNCOMPRESS);
    saved_use_zlib_compress_ = GetConfig(USE_ZLIB_COMPRESS);
    saved_use_iaa_compress_ = GetConfig(USE_IAA_COMPRESS);
    saved_use_qat_compress_ = GetConfig(USE_QAT_COMPRESS);
    saved_use_igzip_compress_ = GetConfig(USE_IGZIP_COMPRESS);
    // SetCompressPath/SetUncompressPath write these two unconditionally as
    // well, so they must be restored too or this fixture leaks them into later
    // tests and makes the suite order-dependent.
    saved_iaa_prepend_empty_block_ = GetConfig(IAA_PREPEND_EMPTY_BLOCK);
    saved_qat_allow_chunking_ = GetConfig(QAT_COMPRESSION_ALLOW_CHUNKING);

    input_ = GenerateBlock(kFlushGateInputLength, compressible_block);
    ASSERT_NE(input_, nullptr);
    compressed_ = BuildMultiBlockRawDeflate(input_, kFlushGateInputLength);
    ASSERT_GT(compressed_.size(), 0u);
  }

  void TearDown() override {
    DestroyBlock(input_);
    SetConfig(USE_ZLIB_UNCOMPRESS, saved_use_zlib_uncompress_);
    SetConfig(USE_IAA_UNCOMPRESS, saved_use_iaa_uncompress_);
    SetConfig(USE_QAT_UNCOMPRESS, saved_use_qat_uncompress_);
    SetConfig(USE_IGZIP_UNCOMPRESS, saved_use_igzip_uncompress_);
    SetConfig(USE_ZLIB_COMPRESS, saved_use_zlib_compress_);
    SetConfig(USE_IAA_COMPRESS, saved_use_iaa_compress_);
    SetConfig(USE_QAT_COMPRESS, saved_use_qat_compress_);
    SetConfig(USE_IGZIP_COMPRESS, saved_use_igzip_compress_);
    SetConfig(IAA_PREPEND_EMPTY_BLOCK, saved_iaa_prepend_empty_block_);
    SetConfig(QAT_COMPRESSION_ALLOW_CHUNKING, saved_qat_allow_chunking_);
  }

  // Runs `flush` once with every accelerator enabled in turn and once on zlib,
  // asserting the accelerator run is indistinguishable from the zlib control.
  void ExpectMatchesZlibControl(int flush) {
    SetUncompressPath(ZLIB, false, false);
    std::string control_output;
    const std::vector<InflateStep> control = RunInflateSteps(
        compressed_, flush, kFlushGateInputLength, &control_output, nullptr);
    ASSERT_EQ(control_output.size(), kFlushGateInputLength);
    ASSERT_GT(control.size(), 1u)
        << "control must return more than once, or the early return this test "
           "checks for is not observable";

    for (const ExecutionPath path : ConfiguredAcceleratorPaths()) {
      SetUncompressPath(path, /*zlib_fallback=*/true, false);
      std::string output;
      ExecutionPath path_after_first = UNDEFINED;
      const std::vector<InflateStep> steps =
          RunInflateSteps(compressed_, flush, kFlushGateInputLength, &output,
                          &path_after_first);

      EXPECT_EQ(path_after_first, ZLIB)
          << PathLabel(path) << ": flush " << flush
          << " must pin the stream to zlib";
      // Deliberately not ASSERT_*: that would return from this helper and leave
      // the remaining paths unchecked, so one failing backend would mask the
      // others. Skip only the indexed comparison below, which needs the sizes
      // to agree to stay in bounds.
      if (steps.size() != control.size()) {
        ADD_FAILURE() << PathLabel(path) << ": call count must match plain zlib"
                      << " (got " << steps.size() << ", expected "
                      << control.size() << ")";
        continue;
      }
      for (size_t i = 0; i < steps.size(); i++) {
        EXPECT_EQ(steps[i].ret, control[i].ret)
            << PathLabel(path) << ": return code differs at call " << i;
        EXPECT_EQ(steps[i].total_in, control[i].total_in)
            << PathLabel(path) << ": total_in differs at call " << i;
        EXPECT_EQ(steps[i].total_out, control[i].total_out)
            << PathLabel(path) << ": total_out differs at call " << i;
        EXPECT_EQ(steps[i].data_type, control[i].data_type)
            << PathLabel(path) << ": data_type differs at call " << i;
      }
      EXPECT_EQ(output, control_output)
          << PathLabel(path) << ": decompressed bytes differ";
    }
  }

  // With every backend compiled out there is no accelerator to gate, so the
  // per-path loops below would iterate zero times and report a pass having
  // asserted nothing. Skip instead: public CI builds exactly that config, and a
  // vacuous pass there would hide a regression. Only the mid-stream IGZIP test
  // is meaningful without this, and it is already #ifdef'd.
  void SkipIfNoAcceleratorConfigured() {
    if (ConfiguredAcceleratorPaths().empty()) {
      GTEST_SKIP() << "no accelerator backend compiled in; nothing to gate";
    }
  }

  char* input_ = nullptr;
  std::string compressed_;

 private:
  uint32_t saved_use_zlib_uncompress_ = 0;
  uint32_t saved_use_iaa_uncompress_ = 0;
  uint32_t saved_use_qat_uncompress_ = 0;
  uint32_t saved_use_igzip_uncompress_ = 0;
  uint32_t saved_use_zlib_compress_ = 0;
  uint32_t saved_use_iaa_compress_ = 0;
  uint32_t saved_use_qat_compress_ = 0;
  uint32_t saved_use_igzip_compress_ = 0;
  uint32_t saved_iaa_prepend_empty_block_ = 0;
  uint32_t saved_qat_allow_chunking_ = 0;
};

// Z_BLOCK asks inflate() to stop at the next block boundary and to report the
// bit position in data_type. Offloaded, the call used to return the whole
// stream with data_type untouched.
TEST_F(InflateFlushGateTest, ZBlockPinsStreamToZlibAndReturnsAtBlockBoundary) {
  SkipIfNoAcceleratorConfigured();
  ExpectMatchesZlibControl(Z_BLOCK);
}

// Z_TREES stops at every block *header* too, and adds 256 to data_type when it
// does -- the bit that distinguishes it from Z_BLOCK.
TEST_F(InflateFlushGateTest, ZTreesPinsStreamToZlibAndReportsBlockHeaderEnd) {
  SkipIfNoAcceleratorConfigured();
  ExpectMatchesZlibControl(Z_TREES);

  SetUncompressPath(ZLIB, false, false);
  const std::vector<InflateStep> control = RunInflateSteps(
      compressed_, Z_TREES, kFlushGateInputLength, nullptr, nullptr);
  bool saw_block_header_end = false;
  for (const InflateStep& step : control) {
    if (step.data_type & 256) {
      saw_block_header_end = true;
      break;
    }
  }
  ASSERT_TRUE(saw_block_header_end)
      << "Z_TREES must report the end of a block header at least once, or "
         "ExpectMatchesZlibControl is comparing against a control that never "
         "exercises the 256 bit";
}

// The pin is stream-wide, not per call: a caller that tracks bit positions must
// not have the stream migrated onto an accelerator by a later Z_NO_FLUSH call,
// which would silently stop updating data_type mid-stream.
TEST_F(InflateFlushGateTest, ZBlockPinIsStickyForRemainderOfStream) {
  SkipIfNoAcceleratorConfigured();
  // One void helper per path so a failing ASSERT_* returns from the helper
  // rather than from the test body, which would leave the remaining backends
  // unchecked.
  const auto check = [this](ExecutionPath path) {
    SetUncompressPath(path, /*zlib_fallback=*/true, false);

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

    std::vector<Bytef> buffer(kFlushGateInputLength + 4096);
    stream.next_in =
        reinterpret_cast<Bytef*>(const_cast<char*>(compressed_.data()));
    stream.avail_in = static_cast<uInt>(compressed_.size());
    stream.next_out = buffer.data();
    stream.avail_out = static_cast<uInt>(buffer.size());

    ASSERT_EQ(inflate(&stream, Z_BLOCK), Z_OK) << PathLabel(path);
    ASSERT_EQ(GetInflateExecutionPath(&stream), ZLIB) << PathLabel(path);
    EXPECT_LT(stream.total_out, kFlushGateInputLength)
        << PathLabel(path) << ": Z_BLOCK must stop before the whole stream";

    int ret = Z_OK;
    for (int i = 0; i < 64 && ret != Z_STREAM_END; i++) {
      ret = inflate(&stream, Z_NO_FLUSH);
      ASSERT_GE(ret, Z_OK) << PathLabel(path);
      EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB)
          << PathLabel(path)
          << ": a later Z_NO_FLUSH call must not move the stream off zlib";
    }
    EXPECT_EQ(ret, Z_STREAM_END) << PathLabel(path);
    EXPECT_EQ(stream.total_out, kFlushGateInputLength) << PathLabel(path);
    EXPECT_EQ(memcmp(buffer.data(), input_, kFlushGateInputLength), 0)
        << PathLabel(path);

    inflateEnd(&stream);
  };

  for (const ExecutionPath path : ConfiguredAcceleratorPaths()) {
    check(path);
  }
}

// The gate pins the path rather than only clearing per-call availability, which
// is what lets the stream reach zlib with use_zlib_uncompress=0: the request
// was never offloadable, so this is not a fallback. Under per-call clearing the
// same configuration would return Z_DATA_ERROR. This is the regression guard
// for that design decision.
TEST_F(InflateFlushGateTest, ZBlockSucceedsWithZlibUncompressDisabled) {
  SkipIfNoAcceleratorConfigured();
  // See ZBlockPinIsStickyForRemainderOfStream for why each path gets its own
  // helper invocation rather than sharing the loop body.
  const auto check = [this](ExecutionPath path) {
    SetUncompressPath(path, /*zlib_fallback=*/false, false);
    ASSERT_EQ(GetConfig(USE_ZLIB_UNCOMPRESS), 0u);

    std::string output;
    ExecutionPath path_after_first = UNDEFINED;
    const std::vector<InflateStep> steps =
        RunInflateSteps(compressed_, Z_BLOCK, kFlushGateInputLength, &output,
                        &path_after_first);

    ASSERT_FALSE(steps.empty());
    EXPECT_EQ(steps.front().ret, Z_OK)
        << PathLabel(path)
        << ": Z_BLOCK must not fail when use_zlib_uncompress=0";
    EXPECT_EQ(path_after_first, ZLIB) << PathLabel(path);
    EXPECT_EQ(steps.back().ret, Z_STREAM_END) << PathLabel(path);
    ASSERT_EQ(output.size(), kFlushGateInputLength) << PathLabel(path);
    EXPECT_EQ(memcmp(output.data(), input_, kFlushGateInputLength), 0)
        << PathLabel(path);
  };

  for (const ExecutionPath path : ConfiguredAcceleratorPaths()) {
    check(path);
  }
}

#ifdef USE_IGZIP
// An IGZIP stream already in flight is exempt from the gate: ISA-L holds
// unflushed inflate state that cannot be handed to zlib without corrupting the
// output, so the stream stays on IGZIP and Z_BLOCK behaves as Z_NO_FLUSH. The
// accepted residual is over-delivery plus an unwritten data_type -- never wrong
// bytes. Mirrors ZBlockMidStreamStaysOnIGZIPAndRoundTrips on the deflate side.
TEST_F(InflateFlushGateTest, ZBlockMidStreamStaysOnIGZIPAndRoundTrips) {
  SetUncompressPath(IGZIP, /*zlib_fallback=*/true, false);

  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);

  std::vector<Bytef> buffer(kFlushGateInputLength + 4096);
  stream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(compressed_.data()));
  stream.next_out = buffer.data();
  stream.avail_out = static_cast<uInt>(buffer.size());

  // Land the stream on IGZIP with a flush value the gate allows, feeding only
  // part of the input so ISA-L is left holding state.
  const size_t first_chunk = compressed_.size() / 4;
  ASSERT_GT(first_chunk, 0u);
  stream.avail_in = static_cast<uInt>(first_chunk);
  const int first_ret = inflate(&stream, Z_NO_FLUSH);
  ASSERT_GE(first_ret, Z_OK);
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);

  // Now switch to Z_BLOCK mid-stream: the exemption keeps it on IGZIP.
  stream.avail_in = static_cast<uInt>(compressed_.size() - first_chunk);
  int ret = inflate(&stream, Z_BLOCK);
  ASSERT_GE(ret, Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP)
      << "an active IGZIP stream must not be handed to zlib mid-stream";

  for (int i = 0; i < 64 && ret != Z_STREAM_END; i++) {
    ret = inflate(&stream, Z_BLOCK);
    ASSERT_GE(ret, Z_OK);
  }
  EXPECT_EQ(ret, Z_STREAM_END);

  // The bytes must still be exact -- only the return timing differs.
  ASSERT_EQ(stream.total_out, kFlushGateInputLength);
  EXPECT_EQ(memcmp(buffer.data(), input_, kFlushGateInputLength), 0);

  inflateEnd(&stream);
}
#endif  // USE_IGZIP

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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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
#ifdef USE_IGZIP
                        ,
                        IGZIP
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

#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)
class DictionaryMidstreamFallbackRegressionTest : public ::testing::Test {};

static void RunMidstreamSetDictionaryRegression(ExecutionPath accel_path) {
  SetCompressPath(accel_path, true, false, false);
  SetUncompressPath(ZLIB, false, false);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  std::string first_chunk;
  std::string second_chunk;
  for (int i = 0; i < 256; ++i) {
    first_chunk += "first:";
    first_chunk += std::to_string(i);
    first_chunk += ":abcdefghijklmnopqrstuvwxyz|";

    second_chunk += "second:";
    second_chunk += std::to_string(i);
    second_chunk += ":0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ|";
  }
  const std::string expected = first_chunk + second_chunk;

  z_stream cstream;
  memset(&cstream, 0, sizeof(cstream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<unsigned char> compressed(compressBound(expected.size()) + 512);
  cstream.next_out = compressed.data();
  cstream.avail_out = static_cast<unsigned int>(compressed.size());

  cstream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(first_chunk.data()));
  cstream.avail_in = static_cast<unsigned int>(first_chunk.size());
  int ret = deflate(&cstream, Z_NO_FLUSH);
  ASSERT_TRUE(ret == Z_OK || ret == Z_BUF_ERROR);
  const ExecutionPath observed_before_dict = GetDeflateExecutionPath(&cstream);
  EXPECT_TRUE(observed_before_dict == accel_path ||
              observed_before_dict == ZLIB);

  const unsigned char dict[] = "midstream-dictionary";
  const int dict_ret =
      deflateSetDictionary(&cstream, dict, static_cast<uInt>(sizeof(dict) - 1));

  // zlib semantics require dictionary to be set before any deflate output.
  // Accepting this call midstream can desynchronize accelerator and zlib state.
  EXPECT_EQ(dict_ret, Z_STREAM_ERROR);
  // Rejected dictionary update must not mutate active stream path.
  EXPECT_EQ(GetDeflateExecutionPath(&cstream), observed_before_dict);

  cstream.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(second_chunk.data()));
  cstream.avail_in = static_cast<unsigned int>(second_chunk.size());
  for (int guard = 0; guard < 128; ++guard) {
    ret = deflate(&cstream, Z_FINISH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);

  const size_t compressed_size = compressed.size() - cstream.avail_out;
  ASSERT_GT(compressed_size, 0u);
  deflateEnd(&cstream);

  z_stream dstream;
  memset(&dstream, 0, sizeof(dstream));
  ASSERT_EQ(inflateInit2(&dstream, 15), Z_OK);

  std::vector<unsigned char> uncompressed(expected.size() + 1024);
  dstream.next_in = compressed.data();
  dstream.avail_in = static_cast<unsigned int>(compressed_size);
  dstream.next_out = uncompressed.data();
  dstream.avail_out = static_cast<unsigned int>(uncompressed.size());

  for (int guard = 0; guard < 128; ++guard) {
    ret = inflate(&dstream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  ASSERT_EQ(ret, Z_STREAM_END);
  const size_t produced = uncompressed.size() - dstream.avail_out;
  ASSERT_EQ(produced, expected.size());
  EXPECT_EQ(memcmp(uncompressed.data(), expected.data(), expected.size()), 0);

  inflateEnd(&dstream);
}

#ifdef USE_IGZIP
TEST_F(DictionaryMidstreamFallbackRegressionTest,
       IGZIPRejectsMidstreamSetDictionary) {
  RunMidstreamSetDictionaryRegression(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(DictionaryMidstreamFallbackRegressionTest,
       QATRejectsMidstreamSetDictionary) {
  RunMidstreamSetDictionaryRegression(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(DictionaryMidstreamFallbackRegressionTest,
       IAARejectsMidstreamSetDictionary) {
  RunMidstreamSetDictionaryRegression(IAA);
}
#endif
#endif

#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)
class DeflateParamsRegressionTest : public ::testing::Test {};

// deflateParams() may lower the level to Z_NO_COMPRESSION after deflateInit*,
// which asks for stored blocks that no backend can emit.  Before deflateParams
// was intercepted, DeflateSettings::level was written only at init, so the
// level-0 gate in deflate() never saw the change and the data was silently
// compressed instead of stored -- on every backend, and with no error returned.
//
// zlib_fallback is deliberately false (use_zlib_compress == 0): a request that
// was never offloadable has to reach orig_deflate by being pinned to ZLIB, not
// by falling back, so this also guards the pin-vs-clear distinction.
static void RunDeflateParamsLevelZeroRegression(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  // Output buffer is set up first: zlib's deflateParams() may need to emit the
  // input compressed so far and returns Z_BUF_ERROR when it cannot.
  ASSERT_EQ(deflateParams(&stream, Z_NO_COMPRESSION, Z_DEFAULT_STRATEGY), Z_OK);

  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - stream.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);
  deflateEnd(&stream);

  // Stored output must exceed its input.  No accelerator can fake this, which
  // is what makes it the oracle for "level 0 was actually honored".
  EXPECT_GT(produced, input_length);
  EXPECT_LT(produced, input_length + 1024);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(
      ZlibUncompress(reinterpret_cast<const char*>(output.data()), produced,
                     input_length, &uncompressed, &uncompressed_length,
                     &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
      Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);
  DestroyBlock(uncompressed);

  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(DeflateParamsRegressionTest, IGZIPLevelZeroAfterInitStoresInput) {
  RunDeflateParamsLevelZeroRegression(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(DeflateParamsRegressionTest, QATLevelZeroAfterInitStoresInput) {
  RunDeflateParamsLevelZeroRegression(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(DeflateParamsRegressionTest, IAALevelZeroAfterInitStoresInput) {
  RunDeflateParamsLevelZeroRegression(IAA);
}
#endif

// The mirror image of the level-0 gate: a deflateParams() call zlib *rejected*
// left the parameters unchanged, so recording it would make path selection
// disagree with the level zlib is really using.  The level here is legal and
// only the strategy is out of range, so an ungated write would record level 0
// and pin an otherwise offloadable stream to zlib, storing the input instead of
// compressing it.
//
// Z_STREAM_ERROR rather than the Z_BUF_ERROR the same gate also covers: zlib
// only returns Z_BUF_ERROR from deflateParams() when it cannot flush what its
// own deflate state has buffered, and an offloaded stream never advances that
// state (on IGZIP zlib's deflate() is never called, so last_flush stays at its
// post-init value and the flushing branch is skipped entirely).  Both
// rejections run through the one `ret == Z_OK` branch under test.
static void RunDeflateParamsRejectedChangeNotRecorded(
    ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  const int invalid_strategy = Z_FIXED + 1;
  ASSERT_NE(deflateParams(&stream, Z_NO_COMPRESSION, invalid_strategy), Z_OK);

  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - stream.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&stream), accel_path);
  deflateEnd(&stream);

  // Had the rejected level 0 been recorded, the stream would have been pinned
  // to zlib and emitted stored blocks, which exceed their input.
  EXPECT_LT(produced, input_length);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(
      ZlibUncompress(reinterpret_cast<const char*>(output.data()), produced,
                     input_length, &uncompressed, &uncompressed_length,
                     &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
      Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);
  DestroyBlock(uncompressed);

  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(DeflateParamsRegressionTest, IGZIPRejectedParamsChangeNotRecorded) {
  RunDeflateParamsRejectedChangeNotRecorded(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(DeflateParamsRegressionTest, QATRejectedParamsChangeNotRecorded) {
  RunDeflateParamsRejectedChangeNotRecorded(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(DeflateParamsRegressionTest, IAARejectedParamsChangeNotRecorded) {
  RunDeflateParamsRejectedChangeNotRecorded(IAA);
}
#endif

#ifdef USE_IGZIP
// The level gate must not over-trigger: a legal level change still describes an
// offloadable stream, so the path must stay on the accelerator.
TEST_F(DeflateParamsRegressionTest, LegalLevelChangeStaysOffloadable) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  ASSERT_EQ(deflateParams(&stream, 1, Z_DEFAULT_STRATEGY), Z_OK);

  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - stream.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&stream), IGZIP);
  deflateEnd(&stream);

  EXPECT_LT(produced, input_length);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(
      ZlibUncompress(reinterpret_cast<const char*>(output.data()), produced,
                     input_length, &uncompressed, &uncompressed_length,
                     &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
      Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);
  DestroyBlock(uncompressed);

  DestroyBlock(input);
}

// A stream ISA-L has already started must NOT be pinned to zlib when the level
// drops to 0 mid-stream.  ISA-L has emitted a header plus compressed data and
// holds unflushed state, so handing it to a zlib deflate state that was never
// fed writes a second header: measured Z_DATA_ERROR with only the pre-switch
// bytes recoverable.  Staying on IGZIP leaves the new level unhonored -- a
// deliberate, documented residual -- but keeps the output valid deflate.
TEST_F(DeflateParamsRegressionTest, MidstreamLevelZeroKeepsStreamIntact) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t half_length = 32 * 1024;
  const size_t input_length = 2 * half_length;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, input_length) + 4096);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  // A real flush is what makes ISA-L take ownership of the stream.
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(half_length);
  ASSERT_EQ(deflate(&stream, Z_SYNC_FLUSH), Z_OK);
  ASSERT_EQ(GetDeflateExecutionPath(&stream), IGZIP);

  ASSERT_EQ(deflateParams(&stream, Z_NO_COMPRESSION, Z_DEFAULT_STRATEGY), Z_OK);

  stream.next_in = reinterpret_cast<Bytef*>(input) + half_length;
  stream.avail_in = static_cast<uInt>(half_length);
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - stream.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&stream), IGZIP);
  deflateEnd(&stream);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(
      ZlibUncompress(reinterpret_cast<const char*>(output.data()), produced,
                     input_length, &uncompressed, &uncompressed_length,
                     &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
      Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);
  DestroyBlock(uncompressed);

  DestroyBlock(input);
}

// Compresses the whole buffer on a fresh IGZIP stream at |level| in one
// Z_FINISH call.  This is the control the two reset-ordering tests below
// compare against, so a stream that has to be rebuilt at a new level must
// reproduce it byte for byte.  ASSERT_* expands to a bare `return;`, which a
// value-returning helper cannot use, so failures go through ADD_FAILURE() and
// an empty result.
static std::vector<Bytef> CompressWholeOnIgzipAtLevel(const char* input,
                                                      size_t input_length,
                                                      int level) {
  z_stream local;
  memset(&local, 0, sizeof(z_stream));
  if (deflateInit2(&local, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) !=
      Z_OK) {
    ADD_FAILURE() << "deflateInit2 failed at level " << level;
    return {};
  }
  std::vector<Bytef> out(deflateBound(&local, input_length) + 4096);
  local.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input));
  local.avail_in = static_cast<uInt>(input_length);
  local.next_out = out.data();
  local.avail_out = static_cast<uInt>(out.size());
  const int ret = deflate(&local, Z_FINISH);
  const size_t produced = out.size() - local.avail_out;
  const ExecutionPath path = GetDeflateExecutionPath(&local);
  deflateEnd(&local);
  if (ret != Z_STREAM_END || path != IGZIP) {
    ADD_FAILURE() << "level " << level << ": ret " << ret << ", path " << path;
    return {};
  }
  out.resize(produced);
  return out;
}

// Recording the level is not sufficient on its own: deflateReset() has to give
// up an ISA-L stream that was built for a level deflateParams() has since
// changed.  isal_deflate_reset() deliberately preserves level and level_buf,
// and deflate() only builds a new ISA-L stream when isal_strm is null, so the
// stream after the reset used to run at the level the FIRST stream was built
// for -- measured 48335 bytes where a fresh level-1 stream produces 51021.
//
// The oracle is byte equality against a fresh stream at the new level, with the
// two controls asserted to differ so the comparison cannot pass vacuously on
// input that compresses identically at both levels.
TEST_F(DeflateParamsRegressionTest, ResetAfterLevelChangeRebuildsIgzipStream) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  const size_t half_length = input_length / 2;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  const std::vector<Bytef> control_level1 =
      CompressWholeOnIgzipAtLevel(input, input_length, 1);
  const std::vector<Bytef> control_level9 =
      CompressWholeOnIgzipAtLevel(input, input_length, 9);
  ASSERT_FALSE(control_level1.empty());
  ASSERT_FALSE(control_level9.empty());
  ASSERT_NE(control_level1, control_level9);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> first(deflateBound(&stream, input_length) + 4096);
  stream.next_out = first.data();
  stream.avail_out = static_cast<uInt>(first.size());

  // A real flush is what makes ISA-L take ownership, so the ISA-L stream is
  // genuinely built at level 9 before the level changes underneath it.
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(half_length);
  ASSERT_EQ(deflate(&stream, Z_SYNC_FLUSH), Z_OK);
  ASSERT_EQ(GetDeflateExecutionPath(&stream), IGZIP);

  ASSERT_EQ(deflateParams(&stream, 1, Z_DEFAULT_STRATEGY), Z_OK);

  stream.next_in = reinterpret_cast<Bytef*>(input) + half_length;
  stream.avail_in = static_cast<uInt>(input_length - half_length);
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);

  // After the reset this is a new stream, and the recorded level is 1.
  ASSERT_EQ(deflateReset(&stream), Z_OK);
  std::vector<Bytef> after(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = after.data();
  stream.avail_out = static_cast<uInt>(after.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = after.size() - stream.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&stream), IGZIP);
  deflateEnd(&stream);
  after.resize(produced);

  EXPECT_EQ(after, control_level1);
  EXPECT_NE(after, control_level9);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(
      ZlibUncompress(reinterpret_cast<const char*>(after.data()), produced,
                     input_length, &uncompressed, &uncompressed_length,
                     &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
      Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);
  DestroyBlock(uncompressed);

  DestroyBlock(input);
}

// The same stale ISA-L level through the opposite ordering, which the
// deflateReset() half above cannot catch: reset FIRST, and the recorded level
// still matches what the ISA-L stream was built for at that moment, so the
// stream is legitimately kept.  deflateParams() then moves the level under a
// stream deflate() reuses as-is, which used to leave the next stream running at
// the old level -- measured 48335 bytes for a level-1 request where a fresh
// level-1 stream produces 51021.  Same oracle as above; the two controls are
// asserted to differ so it cannot pass vacuously.
TEST_F(DeflateParamsRegressionTest, ParamsAfterResetRebuildsIgzipStream) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  const std::vector<Bytef> control_level1 =
      CompressWholeOnIgzipAtLevel(input, input_length, 1);
  const std::vector<Bytef> control_level9 =
      CompressWholeOnIgzipAtLevel(input, input_length, 9);
  ASSERT_FALSE(control_level1.empty());
  ASSERT_FALSE(control_level9.empty());
  ASSERT_NE(control_level1, control_level9);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY),
            Z_OK);

  // A complete stream first, so ISA-L really owns one built at level 9.
  std::vector<Bytef> first(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = first.data();
  stream.avail_out = static_cast<uInt>(first.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  ASSERT_EQ(GetDeflateExecutionPath(&stream), IGZIP);

  // The reset comes before the level change, so it has nothing to act on.
  ASSERT_EQ(deflateReset(&stream), Z_OK);
  ASSERT_EQ(deflateParams(&stream, 1, Z_DEFAULT_STRATEGY), Z_OK);

  std::vector<Bytef> after(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = after.data();
  stream.avail_out = static_cast<uInt>(after.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = after.size() - stream.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&stream), IGZIP);
  deflateEnd(&stream);
  after.resize(produced);

  EXPECT_EQ(after, control_level1);
  EXPECT_NE(after, control_level9);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(
      ZlibUncompress(reinterpret_cast<const char*>(after.data()), produced,
                     input_length, &uncompressed, &uncompressed_length,
                     &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
      Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);
  DestroyBlock(uncompressed);

  DestroyBlock(input);
}
#endif  // USE_IGZIP
#endif  // USE_IGZIP || USE_QAT || USE_IAA

#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)
class StreamCopyRegressionTest : public ::testing::Test {};

// deflateCopy()/inflateCopy() duplicate zlib's stream state, but the shim keeps
// its own per-stream state in maps keyed by z_streamp, so before these were
// intercepted the copy had no entry at all.  Since the null guards landed, an
// untracked z_streamp no longer crashes -- it silently runs on orig_deflate /
// orig_inflate instead, which loses the offload outright and, with the source
// midstream on IGZIP, produced output that does not inflate (measured
// Z_DATA_ERROR with only the pre-copy bytes recoverable).
//
// The simplest case: copy a stream that has not been used yet, then compress
// independently on both.  Without registration the copy degrades to zlib.
static void RunDeflateCopyPreservesOffload(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&source, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  z_stream copy;
  memset(&copy, 0, sizeof(z_stream));
  ASSERT_EQ(deflateCopy(&copy, &source), Z_OK);

  // The copy is compressed first, so nothing it does can be explained by state
  // the source built up afterwards.
  const size_t bound = deflateBound(&source, input_length) + 4096;
  std::vector<Bytef> copy_output(bound);
  copy.next_in = reinterpret_cast<Bytef*>(input);
  copy.avail_in = static_cast<uInt>(input_length);
  copy.next_out = copy_output.data();
  copy.avail_out = static_cast<uInt>(copy_output.size());
  ASSERT_EQ(deflate(&copy, Z_FINISH), Z_STREAM_END);
  const size_t copy_produced = copy_output.size() - copy.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&copy), accel_path);
  ASSERT_EQ(deflateEnd(&copy), Z_OK);
  copy_output.resize(copy_produced);

  std::vector<Bytef> source_output(bound);
  source.next_in = reinterpret_cast<Bytef*>(input);
  source.avail_in = static_cast<uInt>(input_length);
  source.next_out = source_output.data();
  source.avail_out = static_cast<uInt>(source_output.size());
  ASSERT_EQ(deflate(&source, Z_FINISH), Z_STREAM_END);
  const size_t source_produced = source_output.size() - source.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&source), accel_path);
  ASSERT_EQ(deflateEnd(&source), Z_OK);
  source_output.resize(source_produced);

  // Both streams started from the same point and were given the same input, so
  // the copy has to be indistinguishable from the source it came from.
  EXPECT_EQ(copy_output, source_output);

  z_stream check;
  memset(&check, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&check, 15), Z_OK);
  std::vector<Bytef> uncompressed(input_length + 1024);
  check.next_in = copy_output.data();
  check.avail_in = static_cast<uInt>(copy_output.size());
  check.next_out = uncompressed.data();
  check.avail_out = static_cast<uInt>(uncompressed.size());
  ASSERT_EQ(inflate(&check, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(check.total_out, input_length);
  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);
  ASSERT_EQ(inflateEnd(&check), Z_OK);

  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(StreamCopyRegressionTest, IGZIPDeflateCopyPreservesOffload) {
  RunDeflateCopyPreservesOffload(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(StreamCopyRegressionTest, QATDeflateCopyPreservesOffload) {
  RunDeflateCopyPreservesOffload(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(StreamCopyRegressionTest, IAADeflateCopyPreservesOffload) {
  RunDeflateCopyPreservesOffload(IAA);
}
#endif

// A stream pinned to ZLIB must hand that pin to its copy rather than let the
// copy re-run path selection.  A preset dictionary is the case where it shows:
// the dictionary lives in zlib's deflate state, which deflateCopy duplicates,
// but no backend supports one, so a copy left UNDEFINED would offload the very
// next call and compress without the dictionary at all.  The oracle is that the
// copy's output cannot be inflated without the dictionary; an offloaded copy
// produces a stream that needs none.
//
// (The level-0 pin cannot serve here: it is recomputed per call from the
// recorded level, so it re-applies on the copy even when nothing is inherited.)
static void RunDeflateCopyInheritsZlibPin(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);
  const uint32_t saved_ignore_dictionary = GetConfig(IGNORE_ZLIB_DICTIONARY);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  const unsigned char dict[] = "stream-copy-preset-dictionary";
  const uInt dict_length = static_cast<uInt>(sizeof(dict) - 1);

  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&source, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  ASSERT_EQ(deflateSetDictionary(&source, dict, dict_length), Z_OK);
  ASSERT_EQ(GetDeflateExecutionPath(&source), ZLIB);

  z_stream copy;
  memset(&copy, 0, sizeof(z_stream));
  ASSERT_EQ(deflateCopy(&copy, &source), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&copy), ZLIB);

  std::vector<Bytef> output(deflateBound(&source, input_length) + 4096);
  copy.next_in = reinterpret_cast<Bytef*>(input);
  copy.avail_in = static_cast<uInt>(input_length);
  copy.next_out = output.data();
  copy.avail_out = static_cast<uInt>(output.size());
  ASSERT_EQ(deflate(&copy, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - copy.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&copy), ZLIB);
  ASSERT_EQ(deflateEnd(&copy), Z_OK);
  ASSERT_EQ(deflateEnd(&source), Z_OK);

  z_stream check;
  memset(&check, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&check, 15), Z_OK);
  std::vector<Bytef> uncompressed(input_length + 1024);
  check.next_in = output.data();
  check.avail_in = static_cast<uInt>(produced);
  check.next_out = uncompressed.data();
  check.avail_out = static_cast<uInt>(uncompressed.size());

  // FDICT in the zlib header is what proves the copy really used zlib's
  // dictionary state instead of being handed to an accelerator.
  ASSERT_EQ(inflate(&check, Z_NO_FLUSH), Z_NEED_DICT);
  ASSERT_EQ(inflateSetDictionary(&check, dict, dict_length), Z_OK);
  int ret = Z_OK;
  for (int guard = 0; guard < 128; ++guard) {
    ret = inflate(&check, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(check.total_out, input_length);
  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);
  ASSERT_EQ(inflateEnd(&check), Z_OK);

  DestroyBlock(input);
  SetConfig(IGNORE_ZLIB_DICTIONARY, saved_ignore_dictionary);
}

#ifdef USE_IGZIP
TEST_F(StreamCopyRegressionTest, IGZIPDeflateCopyInheritsZlibPin) {
  RunDeflateCopyInheritsZlibPin(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(StreamCopyRegressionTest, QATDeflateCopyInheritsZlibPin) {
  RunDeflateCopyInheritsZlibPin(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(StreamCopyRegressionTest, IAADeflateCopyInheritsZlibPin) {
  RunDeflateCopyInheritsZlibPin(IAA);
}
#endif

// The one case that cannot be copied at all.  ISA-L has emitted a header and
// still holds unflushed state, and isal_zstream::level_buf is cast to a private
// struct holding pointers into its own allocation, so a byte-for-byte copy
// would leave both streams writing into one pending block.  Draining that block
// first is no help either: those bytes belong to the prefix the two streams
// share, and deflateCopy() has no way to hand bytes back to the caller.  So the
// call is refused, and -- because it is refused before orig_deflateCopy runs --
// dest is left exactly as the caller passed it and the source is untouched.
#ifdef USE_IGZIP
TEST_F(StreamCopyRegressionTest, IGZIPRefusesMidstreamDeflateCopy) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t half_length = 32 * 1024;
  const size_t input_length = 2 * half_length;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&source, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&source, input_length) + 4096);
  source.next_out = output.data();
  source.avail_out = static_cast<uInt>(output.size());

  // A real flush is what makes ISA-L take ownership of the stream.
  source.next_in = reinterpret_cast<Bytef*>(input);
  source.avail_in = static_cast<uInt>(half_length);
  ASSERT_EQ(deflate(&source, Z_SYNC_FLUSH), Z_OK);
  ASSERT_EQ(GetDeflateExecutionPath(&source), IGZIP);

  z_stream copy;
  memset(&copy, 0, sizeof(z_stream));
  EXPECT_EQ(deflateCopy(&copy, &source), Z_STREAM_ERROR);
  EXPECT_EQ(GetDeflateExecutionPath(&source), IGZIP);

  // dest was never initialized, so a caller that ignores the return code gets
  // zlib's error on first use rather than output from a half-built stream.
  std::vector<Bytef> copy_output(4096);
  copy.next_in = reinterpret_cast<Bytef*>(input);
  copy.avail_in = static_cast<uInt>(half_length);
  copy.next_out = copy_output.data();
  copy.avail_out = static_cast<uInt>(copy_output.size());
  EXPECT_EQ(deflate(&copy, Z_FINISH), Z_STREAM_ERROR);

  // The refused copy must not have disturbed the source: it finishes normally
  // and its output is one valid deflate stream.
  source.next_in = reinterpret_cast<Bytef*>(input) + half_length;
  source.avail_in = static_cast<uInt>(half_length);
  ASSERT_EQ(deflate(&source, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - source.avail_out;
  EXPECT_EQ(GetDeflateExecutionPath(&source), IGZIP);
  ASSERT_EQ(deflateEnd(&source), Z_OK);

  z_stream check;
  memset(&check, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&check, 15), Z_OK);
  std::vector<Bytef> uncompressed(input_length + 1024);
  check.next_in = output.data();
  check.avail_in = static_cast<uInt>(produced);
  check.next_out = uncompressed.data();
  check.avail_out = static_cast<uInt>(uncompressed.size());
  ASSERT_EQ(inflate(&check, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(check.total_out, input_length);
  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);
  ASSERT_EQ(inflateEnd(&check), Z_OK);

  DestroyBlock(input);
}
#endif  // USE_IGZIP

// The inflate side of the registration gap: a copy taken before the first
// inflate() call has to keep decompressing on the accelerator, not silently
// drop to zlib.
static void RunInflateCopyPreservesOffload(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  // Compressed by the same backend, so the stream is one it can decompress
  // (IAA in particular only takes streams within its own window size).
  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&source, 15), Z_OK);

  z_stream copy;
  memset(&copy, 0, sizeof(z_stream));
  ASSERT_EQ(inflateCopy(&copy, &source), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  copy.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  copy.avail_in = static_cast<uInt>(compressed.size());
  copy.next_out = uncompressed.data();
  copy.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; ++guard) {
    ret = inflate(&copy, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&copy), accel_path);
  EXPECT_EQ(copy.total_out, input_length);
  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);

  ASSERT_EQ(inflateEnd(&copy), Z_OK);
  ASSERT_EQ(inflateEnd(&source), Z_OK);

  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(StreamCopyRegressionTest, IGZIPInflateCopyPreservesOffload) {
  RunInflateCopyPreservesOffload(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(StreamCopyRegressionTest, QATInflateCopyPreservesOffload) {
  RunInflateCopyPreservesOffload(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(StreamCopyRegressionTest, IAAInflateCopyPreservesOffload) {
  RunInflateCopyPreservesOffload(IAA);
}
#endif

#ifdef USE_IGZIP
// Unlike the deflate side, a midstream inflateCopy() is exact: every member of
// ISA-L's inflate_state is a scalar or an inline array, and next_in/next_out
// are re-pointed on every call, so the clone owns no memory the source also
// owns. Both streams must therefore be able to finish the same stream
// independently. Before inflateCopy was intercepted the copy held no ISA-L
// state at all and could not continue: ISA-L had consumed the input into its
// own buffers, so the bytes the copy needed were no longer in avail_in and it
// returned Z_BUF_ERROR for as long as it was called.
TEST_F(StreamCopyRegressionTest, IGZIPMidstreamInflateCopyIsIndependent) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  const size_t prefix_length = 20000;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&source, 15), Z_OK);

  // Capping avail_out short of the full output is what leaves ISA-L midstream,
  // holding both a partly consumed input and undelivered output.
  std::vector<Bytef> source_output(input_length + 1024);
  source.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  source.avail_in = static_cast<uInt>(compressed.size());
  source.next_out = source_output.data();
  source.avail_out = static_cast<uInt>(prefix_length);
  ASSERT_EQ(inflate(&source, Z_NO_FLUSH), Z_OK);
  ASSERT_EQ(GetInflateExecutionPath(&source), IGZIP);
  ASSERT_EQ(source.total_out, prefix_length);

  z_stream copy;
  memset(&copy, 0, sizeof(z_stream));
  ASSERT_EQ(inflateCopy(&copy, &source), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&copy), IGZIP);

  // inflateCopy duplicates next_out along with everything else, so the copy has
  // to be pointed at its own buffer before it writes anything.
  std::vector<Bytef> copy_output(input_length + 1024);
  copy.next_out = copy_output.data();
  copy.avail_out = static_cast<uInt>(copy_output.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; ++guard) {
    ret = inflate(&copy, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  const size_t copy_produced = copy_output.size() - copy.avail_out;
  ASSERT_EQ(copy_produced, input_length - prefix_length);
  EXPECT_EQ(memcmp(copy_output.data(), input + prefix_length, copy_produced),
            0);
  ASSERT_EQ(inflateEnd(&copy), Z_OK);

  // The source has to be just as usable afterwards, which is what rules out the
  // two streams sharing one ISA-L state.
  source.next_out = source_output.data() + prefix_length;
  source.avail_out = static_cast<uInt>(source_output.size() - prefix_length);
  for (int guard = 0; guard < 128; ++guard) {
    ret = inflate(&source, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(source.total_out, input_length);
  EXPECT_EQ(memcmp(source_output.data(), input, input_length), 0);
  ASSERT_EQ(inflateEnd(&source), Z_OK);

  DestroyBlock(input);
}

// The lifetimes have to be independent in the other direction too: ending the
// source frees its ISA-L state, so a copy that had merely borrowed the pointer
// would be reading freed memory here.
TEST_F(StreamCopyRegressionTest, IGZIPInflateCopySurvivesSourceEnd) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  const size_t prefix_length = 20000;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&source, 15), Z_OK);

  std::vector<Bytef> source_output(input_length + 1024);
  source.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  source.avail_in = static_cast<uInt>(compressed.size());
  source.next_out = source_output.data();
  source.avail_out = static_cast<uInt>(prefix_length);
  ASSERT_EQ(inflate(&source, Z_NO_FLUSH), Z_OK);
  ASSERT_EQ(GetInflateExecutionPath(&source), IGZIP);

  z_stream copy;
  memset(&copy, 0, sizeof(z_stream));
  ASSERT_EQ(inflateCopy(&copy, &source), Z_OK);

  std::vector<Bytef> copy_output(input_length + 1024);
  copy.next_out = copy_output.data();
  copy.avail_out = static_cast<uInt>(copy_output.size());

  ASSERT_EQ(inflateEnd(&source), Z_OK);

  int ret = Z_OK;
  for (int guard = 0; guard < 128; ++guard) {
    ret = inflate(&copy, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  const size_t copy_produced = copy_output.size() - copy.avail_out;
  ASSERT_EQ(copy_produced, input_length - prefix_length);
  EXPECT_EQ(memcmp(copy_output.data(), input + prefix_length, copy_produced),
            0);
  ASSERT_EQ(inflateEnd(&copy), Z_OK);

  DestroyBlock(input);
}

// Copying onto a destination that already owns ISA-L state.  Registering the
// copy replaces the destination's entry, and isal_strm is a raw pointer that
// destroying the old entry does not free, so the old ISA-L stream has to be
// released explicitly or it leaks (measured under LSAN at 414,160 bytes per
// deflateCopy and 87,368 -- sizeof(struct inflate_state) -- per inflateCopy,
// over and above the state zlib itself leaks on a copy onto a live stream).
// The leak itself is only visible to a memory checker -- the
// replacement entry reads as owning nothing either way -- so what these two
// cases pin is the other half: the copy owns no ISA-L state of its own, which
// is what keeps the two streams from sharing one, and releasing the old state
// does not disturb the zlib state the copy has to continue from.  Reaching the
// state at all depends on deflateReset()/inflateReset() keeping the ISA-L
// stream.
TEST_F(StreamCopyRegressionTest,
       IGZIPDeflateCopyReleasesDestinationIgzipState) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  const size_t prefix_length = 20000;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream dest;
  memset(&dest, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&dest, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  const size_t bound = deflateBound(&dest, input_length) + 4096;
  std::vector<Bytef> scratch(bound);
  dest.next_in = reinterpret_cast<Bytef*>(input);
  dest.avail_in = static_cast<uInt>(input_length);
  dest.next_out = scratch.data();
  dest.avail_out = static_cast<uInt>(scratch.size());
  ASSERT_EQ(deflate(&dest, Z_FINISH), Z_STREAM_END);
  ASSERT_EQ(GetDeflateExecutionPath(&dest), IGZIP);
  ASSERT_TRUE(DeflateOwnsIgzipState(&dest));

  // The reset keeps the ISA-L stream -- the recorded level has not changed, so
  // there is nothing to rebuild -- which is what makes the leak reachable.
  ASSERT_EQ(deflateReset(&dest), Z_OK);
  ASSERT_TRUE(DeflateOwnsIgzipState(&dest));

  // A source pinned to ZLIB, which is the one kind deflateCopy accepts from a
  // stream that has already been used.
  SetCompressPath(ZLIB, false, false, false);
  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&source, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> source_output(bound);
  source.next_in = reinterpret_cast<Bytef*>(input);
  source.avail_in = static_cast<uInt>(prefix_length);
  source.next_out = source_output.data();
  source.avail_out = static_cast<uInt>(source_output.size());
  ASSERT_EQ(deflate(&source, Z_NO_FLUSH), Z_OK);
  ASSERT_EQ(source.avail_in, 0u);
  ASSERT_EQ(GetDeflateExecutionPath(&source), ZLIB);
  const size_t source_produced = source_output.size() - source.avail_out;

  ASSERT_EQ(deflateCopy(&dest, &source), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&dest), ZLIB);
  EXPECT_FALSE(DeflateOwnsIgzipState(&dest));

  // Finishing on the copy proves the release did not disturb the state the copy
  // is meant to continue from: the prefix the source emitted plus the tail the
  // copy emits has to be the whole input.
  std::vector<Bytef> dest_output(bound);
  dest.next_in = reinterpret_cast<Bytef*>(input) + prefix_length;
  dest.avail_in = static_cast<uInt>(input_length - prefix_length);
  dest.next_out = dest_output.data();
  dest.avail_out = static_cast<uInt>(dest_output.size());
  ASSERT_EQ(deflate(&dest, Z_FINISH), Z_STREAM_END);
  const size_t dest_produced = dest_output.size() - dest.avail_out;
  ASSERT_EQ(deflateEnd(&dest), Z_OK);
  // The source is abandoned with its stream unfinished, which is exactly the
  // case zlib reports Z_DATA_ERROR for; the copy carried the tail.
  ASSERT_EQ(deflateEnd(&source), Z_DATA_ERROR);

  std::vector<Bytef> compressed(source_output.begin(),
                                source_output.begin() + source_produced);
  compressed.insert(compressed.end(), dest_output.begin(),
                    dest_output.begin() + dest_produced);

  z_stream check;
  memset(&check, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&check, 15), Z_OK);
  std::vector<Bytef> uncompressed(input_length + 1024);
  check.next_in = compressed.data();
  check.avail_in = static_cast<uInt>(compressed.size());
  check.next_out = uncompressed.data();
  check.avail_out = static_cast<uInt>(uncompressed.size());
  ASSERT_EQ(inflate(&check, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(check.total_out, input_length);
  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);
  ASSERT_EQ(inflateEnd(&check), Z_OK);

  DestroyBlock(input);
}

// The inflate half.  The source has to be off IGZIP here: an IGZIP source hands
// the copy a freshly cloned inflate_state, so the destination would own ISA-L
// state either way.  That direction is covered by
// IGZIPMidstreamInflateCopyIsIndependent, which fails if the release ever
// reaches the clone instead of the entry it replaced.
TEST_F(StreamCopyRegressionTest,
       IGZIPInflateCopyReleasesDestinationIgzipState) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  const size_t prefix_length = 20000;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream dest;
  memset(&dest, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&dest, 15), Z_OK);

  std::vector<Bytef> scratch(input_length + 1024);
  dest.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  dest.avail_in = static_cast<uInt>(compressed.size());
  dest.next_out = scratch.data();
  dest.avail_out = static_cast<uInt>(scratch.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; ++guard) {
    ret = inflate(&dest, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&dest), IGZIP);
  ASSERT_TRUE(InflateOwnsIgzipState(&dest));

  ASSERT_EQ(inflateReset(&dest), Z_OK);
  ASSERT_TRUE(InflateOwnsIgzipState(&dest));

  SetUncompressPath(ZLIB, false, false);
  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&source, 15), Z_OK);

  std::vector<Bytef> source_output(input_length + 1024);
  source.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  source.avail_in = static_cast<uInt>(compressed.size());
  source.next_out = source_output.data();
  source.avail_out = static_cast<uInt>(prefix_length);
  ASSERT_EQ(inflate(&source, Z_NO_FLUSH), Z_OK);
  ASSERT_EQ(GetInflateExecutionPath(&source), ZLIB);
  const size_t source_produced = source.total_out;

  ASSERT_EQ(inflateCopy(&dest, &source), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&dest), ZLIB);
  EXPECT_FALSE(InflateOwnsIgzipState(&dest));

  // As on the deflate side, the copy has to be able to finish the stream the
  // source was partway through.
  std::vector<Bytef> dest_output(input_length + 1024);
  dest.next_out = dest_output.data();
  dest.avail_out = static_cast<uInt>(dest_output.size());
  ret = Z_OK;
  for (int guard = 0; guard < 128; ++guard) {
    ret = inflate(&dest, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  const size_t dest_produced = dest_output.size() - dest.avail_out;
  ASSERT_EQ(dest_produced, input_length - source_produced);
  EXPECT_EQ(memcmp(dest_output.data(), input + source_produced, dest_produced),
            0);
  ASSERT_EQ(inflateEnd(&dest), Z_OK);
  ASSERT_EQ(inflateEnd(&source), Z_OK);

  DestroyBlock(input);
}
#endif  // USE_IGZIP
#endif  // USE_IGZIP || USE_QAT || USE_IAA

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
  EXPECT_EQ(GetConfig(USE_IGZIP_COMPRESS), 0);
  EXPECT_EQ(GetConfig(USE_IGZIP_UNCOMPRESS), 0);
  EXPECT_EQ(GetConfig(USE_ZLIB_COMPRESS), 1);
  EXPECT_EQ(GetConfig(USE_ZLIB_UNCOMPRESS), 1);
  EXPECT_EQ(GetConfig(LOG_LEVEL), 1);
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

TEST_F(ConfigLoaderTest, MapShardsValidPowerOfTwo) {
  std::string file_content;
  const char* config_path = "/tmp/map_shards_valid_config";
  const uint32_t saved_shards = GetConfig(MAP_SHARDS);
  std::ofstream config_file(config_path);
  config_file << "map_shards=128\n";
  config_file.close();
  EXPECT_TRUE(LoadConfigFile(file_content, config_path));
  EXPECT_EQ(GetConfig(MAP_SHARDS), 128u);
  std::remove(config_path);
  SetConfig(MAP_SHARDS, saved_shards);
}

TEST_F(ConfigLoaderTest, MapShardsInvalidNonPowerOfTwo) {
  std::string file_content;
  const char* config_path = "/tmp/map_shards_invalid_config";
  const uint32_t saved_shards = GetConfig(MAP_SHARDS);
  std::ofstream config_file(config_path);
  config_file << "map_shards=100\n";
  config_file.close();
  // File is valid; invalid value is rejected by the power-of-2 validator and
  // the setting stays at its default.
  EXPECT_TRUE(LoadConfigFile(file_content, config_path));
  EXPECT_EQ(GetConfig(MAP_SHARDS), saved_shards);
  std::remove(config_path);
  SetConfig(MAP_SHARDS, saved_shards);
}

// The shim keeps per-stream state in maps keyed by z_streamp, and every entry
// point that consumes that state has to cope with the entry being absent: a
// stream that was never initialized at all, one whose *Init failed, or a
// gzFile the shim never saw. Before these guards existed each of those cases
// dereferenced a null shared_ptr.
class UnregisteredStreamTest : public ::testing::Test {};

TEST_F(UnregisteredStreamTest, DeflateAfterFailedInitDoesNotCrash) {
  z_stream strm;
  memset(&strm, 0, sizeof(strm));

  // window_bits is invalid, so zlib rejects the stream and the shim registers
  // nothing for it. deflate() therefore finds no entry and has to hand the call
  // to zlib, which reports its own error, rather than dereferencing the miss.
  ASSERT_EQ(deflateInit2(&strm, 6, Z_DEFLATED, 99, 8, Z_DEFAULT_STRATEGY),
            Z_STREAM_ERROR);

  std::vector<uint8_t> input(1024, 'a');
  std::vector<uint8_t> output(4096, 0);
  strm.next_in = input.data();
  strm.avail_in = static_cast<uInt>(input.size());
  strm.next_out = output.data();
  strm.avail_out = static_cast<uInt>(output.size());

  // An app that ignores the failed init and calls deflate anyway must get
  // zlib's error back rather than a segfault.
  EXPECT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_ERROR);
  EXPECT_EQ(GetDeflateExecutionPath(&strm), ZLIB);
}

TEST_F(UnregisteredStreamTest, InflateAfterFailedInitDoesNotCrash) {
  z_stream strm;
  memset(&strm, 0, sizeof(strm));

  ASSERT_EQ(inflateInit2(&strm, 99), Z_STREAM_ERROR);

  std::vector<uint8_t> input(1024, 0);
  std::vector<uint8_t> output(4096, 0);
  strm.next_in = input.data();
  strm.avail_in = static_cast<uInt>(input.size());
  strm.next_out = output.data();
  strm.avail_out = static_cast<uInt>(output.size());

  EXPECT_EQ(inflate(&strm, Z_FINISH), Z_STREAM_ERROR);
  EXPECT_EQ(GetInflateExecutionPath(&strm), ZLIB);
}

// A stream the shim has never seen at all. Reset/SetDictionary/End have to
// tolerate the missing entry too, not just deflate/inflate.
TEST_F(UnregisteredStreamTest, UnknownStreamIsReportedAsZlibPath) {
  z_stream strm;
  memset(&strm, 0, sizeof(strm));

  EXPECT_EQ(GetDeflateExecutionPath(&strm), ZLIB);
  EXPECT_EQ(GetInflateExecutionPath(&strm), ZLIB);
}

TEST_F(UnregisteredStreamTest, ResetAndEndOnUnregisteredStream) {
  z_stream deflate_strm;
  memset(&deflate_strm, 0, sizeof(deflate_strm));
  EXPECT_EQ(deflateReset(&deflate_strm), Z_STREAM_ERROR);
  EXPECT_EQ(deflateEnd(&deflate_strm), Z_STREAM_ERROR);

  z_stream inflate_strm;
  memset(&inflate_strm, 0, sizeof(inflate_strm));
  EXPECT_EQ(inflateReset(&inflate_strm), Z_STREAM_ERROR);
  EXPECT_EQ(inflateEnd(&inflate_strm), Z_STREAM_ERROR);
}

TEST_F(UnregisteredStreamTest, SetDictionaryOnUnregisteredStream) {
  const uint32_t saved = GetConfig(IGNORE_ZLIB_DICTIONARY);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  std::vector<uint8_t> dict(64, 'd');

  z_stream deflate_strm;
  memset(&deflate_strm, 0, sizeof(deflate_strm));
  EXPECT_EQ(deflateSetDictionary(&deflate_strm, dict.data(),
                                 static_cast<uInt>(dict.size())),
            Z_STREAM_ERROR);

  z_stream inflate_strm;
  memset(&inflate_strm, 0, sizeof(inflate_strm));
  EXPECT_EQ(inflateSetDictionary(&inflate_strm, dict.data(),
                                 static_cast<uInt>(dict.size())),
            Z_STREAM_ERROR);

  SetConfig(IGNORE_ZLIB_DICTIONARY, saved);
}

// A stream that fails to init, is then successfully re-initialized, must end up
// registered and usable — the failed attempt must not leave the shim confused
// about the stream.
TEST_F(UnregisteredStreamTest, ReinitAfterFailedInitWorks) {
  z_stream strm;
  memset(&strm, 0, sizeof(strm));

  ASSERT_EQ(deflateInit2(&strm, 6, Z_DEFLATED, 99, 8, Z_DEFAULT_STRATEGY),
            Z_STREAM_ERROR);
  ASSERT_EQ(deflateInit2(&strm, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<uint8_t> input(4096, 'a');
  std::vector<uint8_t> compressed(8192, 0);
  strm.next_in = input.data();
  strm.avail_in = static_cast<uInt>(input.size());
  strm.next_out = compressed.data();
  strm.avail_out = static_cast<uInt>(compressed.size());
  ASSERT_EQ(deflate(&strm, Z_FINISH), Z_STREAM_END);
  const size_t compressed_size = compressed.size() - strm.avail_out;
  ASSERT_EQ(deflateEnd(&strm), Z_OK);

  // Round-trip to confirm the stream really was functional, not just non-fatal.
  z_stream d;
  memset(&d, 0, sizeof(d));
  ASSERT_EQ(inflateInit2(&d, 31), Z_OK);
  std::vector<uint8_t> decompressed(input.size(), 0);
  d.next_in = compressed.data();
  d.avail_in = static_cast<uInt>(compressed_size);
  d.next_out = decompressed.data();
  d.avail_out = static_cast<uInt>(decompressed.size());
  EXPECT_EQ(inflate(&d, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(inflateEnd(&d), Z_OK);
  EXPECT_EQ(decompressed, input);
}

// gzFile entry points on a handle the shim never registered. nullptr is the
// simplest such handle and is what gzopen returns on failure, so an app that
// ignores that failure reaches exactly this path.
TEST_F(UnregisteredStreamTest, GzFunctionsOnUnregisteredFile) {
  std::vector<uint8_t> buf(64, 0);

  // These match what plain zlib returns for a file it cannot act on.
  EXPECT_EQ(gzeof(nullptr), 0);
  EXPECT_EQ(gzread(nullptr, buf.data(), static_cast<unsigned>(buf.size())), -1);
  EXPECT_EQ(gzwrite(nullptr, buf.data(), static_cast<unsigned>(buf.size())), 0);
  EXPECT_EQ(gzclose(nullptr), Z_STREAM_ERROR);
}

// These tests mutate the global path configuration, so restore it in TearDown
// rather than at the end of the test body: ASSERT_* returns from the function,
// which would skip an inline restore and leak the setting into later tests.
class GzipFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_iaa_compress_ = GetConfig(USE_IAA_COMPRESS);
    saved_qat_compress_ = GetConfig(USE_QAT_COMPRESS);
    saved_zlib_compress_ = GetConfig(USE_ZLIB_COMPRESS);
    saved_iaa_uncompress_ = GetConfig(USE_IAA_UNCOMPRESS);
    saved_qat_uncompress_ = GetConfig(USE_QAT_UNCOMPRESS);
    saved_zlib_uncompress_ = GetConfig(USE_ZLIB_UNCOMPRESS);
  }

  void TearDown() override {
    SetConfig(USE_IAA_COMPRESS, saved_iaa_compress_);
    SetConfig(USE_QAT_COMPRESS, saved_qat_compress_);
    SetConfig(USE_ZLIB_COMPRESS, saved_zlib_compress_);
    SetConfig(USE_IAA_UNCOMPRESS, saved_iaa_uncompress_);
    SetConfig(USE_QAT_UNCOMPRESS, saved_qat_uncompress_);
    SetConfig(USE_ZLIB_UNCOMPRESS, saved_zlib_uncompress_);
    remove("file.gz");
  }

 private:
  uint32_t saved_iaa_compress_ = 0;
  uint32_t saved_qat_compress_ = 0;
  uint32_t saved_zlib_compress_ = 0;
  uint32_t saved_iaa_uncompress_ = 0;
  uint32_t saved_qat_uncompress_ = 0;
  uint32_t saved_zlib_uncompress_ = 0;
};

// gzeof has to answer for files the shim handed to zlib as well as the ones it
// decompressed itself. gz->reached_eof is only ever set by the accelerator read
// loop, so on the zlib path it stays false forever and gzeof must defer to
// orig_gzeof. Assert on the return value rather than looping until gzeof is
// true: without the fix the loop form hangs instead of failing.
TEST_F(GzipFileTest, GzeofReportsEofOnZlibPath) {
  // No accelerator enabled means gzwrite/gzread both delegate to zlib, so the
  // file ends up on the ZLIB path.
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(ZLIB, false, false);

  std::vector<char> input(8192, 'a');
  ASSERT_EQ(ZlibCompressGzipFile(input.data(), input.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  // Room to spare, so the loop ends on a zero-length read rather than on a full
  // buffer — zlib only sets its end-of-file indicator once a read runs off the
  // end of the compressed data.
  std::vector<char> output(input.size() + 512, 0);
  size_t read_total = 0;
  int read_ret = 0;
  do {
    read_ret = gzread(fp, output.data() + read_total,
                      static_cast<unsigned>(output.size() - read_total));
    ASSERT_GE(read_ret, 0);
    read_total += static_cast<size_t>(read_ret);
    ASSERT_LE(read_total, output.size());
  } while (read_ret > 0);

  ASSERT_EQ(read_total, input.size());
  output.resize(read_total);
  EXPECT_EQ(output, input);

  // zlib has consumed the whole member, so gzeof must say so.
  EXPECT_NE(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
}

// On the accelerator path reached_eof only records that a read of the file came
// up short, which happens while data_buf/io_buf still hold bytes gzread has not
// returned yet. gzeof reporting EOF there truncates the common
// "while (!gzeof(file)) gzread(...)" loop, so it has to account for the
// buffered data too.
TEST_F(GzipFileTest, GzeofDoesNotReportEofWithBufferedData) {
  // Write with zlib so the file is a plain gzip member, and read back with an
  // accelerator enabled so gzread takes its own buffered path. Which
  // accelerator does not matter; if neither is available the read falls back to
  // zlib and the loop below still has to deliver every byte.
  SetCompressPath(ZLIB, false, false, false);
#if defined(USE_IAA)
  SetUncompressPath(IAA, true, false);
#elif defined(USE_QAT)
  SetUncompressPath(QAT, true, false);
#else
  SetUncompressPath(ZLIB, false, false);
#endif

  std::vector<char> input(8192, 'a');
  ASSERT_EQ(ZlibCompressGzipFile(input.data(), input.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  // Read in chunks smaller than the file so a chunk boundary lands after the
  // file has been fully read but before all of it has been handed back. This is
  // the loop shape the bug breaks: gzeof gates the next read.
  std::vector<char> output;
  std::vector<char> chunk(4096, 0);
  int iterations = 0;
  while (gzeof(fp) == 0) {
    int read_ret =
        gzread(fp, chunk.data(), static_cast<unsigned>(chunk.size()));
    ASSERT_GE(read_ret, 0);
    output.insert(output.end(), chunk.begin(), chunk.begin() + read_ret);
    // The loop must terminate through gzeof, not run away.
    ASSERT_LT(++iterations, 64);
    if (read_ret == 0) {
      break;
    }
  }

  EXPECT_EQ(output.size(), input.size());
  EXPECT_EQ(output, input);

  EXPECT_EQ(gzclose(fp), Z_OK);
}

class ShardedMapTest : public ::testing::Test {};

TEST_F(ShardedMapTest, BasicSetAndGet) {
  ShardedMap<std::string, std::shared_ptr<int>> map;

  std::string key = "test_key";
  auto value = std::make_shared<int>(42);
  int* raw_ptr = value.get();

  map.Set(key, std::move(value));

  auto retrieved = map.Get(key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(*retrieved, 42);
  EXPECT_EQ(retrieved.get(), raw_ptr);

  map.Unset(key);
}

TEST_F(ShardedMapTest, GetNonExistentKey) {
  ShardedMap<std::string, std::shared_ptr<int>> map;
  EXPECT_EQ(map.Get("non_existent"), nullptr);
}

TEST_F(ShardedMapTest, SetOverwritesExistingKey) {
  ShardedMap<std::string, std::shared_ptr<int>> map;

  std::string key = "test_key";
  auto value1 = std::make_shared<int>(100);
  auto value2 = std::make_shared<int>(200);
  int* raw_ptr2 = value2.get();

  map.Set(key, std::move(value1));
  map.Set(key, std::move(value2));

  auto retrieved = map.Get(key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(*retrieved, 200);
  EXPECT_EQ(retrieved.get(), raw_ptr2);

  map.Unset(key);
}

TEST_F(ShardedMapTest, UnsetRemovesKey) {
  ShardedMap<std::string, std::shared_ptr<int>> map;

  std::string key = "test_key";
  auto value = std::make_shared<int>(42);

  map.Set(key, std::move(value));

  auto retrieved_before = map.Get(key);
  ASSERT_NE(retrieved_before, nullptr);

  map.Unset(key);

  EXPECT_EQ(map.Get(key), nullptr);
}

TEST_F(ShardedMapTest, UnsetNonExistentKey) {
  ShardedMap<std::string, std::shared_ptr<int>> map;
  EXPECT_NO_THROW(map.Unset("non_existent"));
}

TEST_F(ShardedMapTest, MultipleKeys) {
  ShardedMap<std::string, std::shared_ptr<int>> map;

  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    auto value = std::make_shared<int>(i * 10);
    map.Set(key, std::move(value));
  }

  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    auto value = map.Get(key);
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
  ShardedMap<std::string, std::shared_ptr<int>> map;

  std::vector<std::string> keys = {"key1",        "key2", "key3", "another_key",
                                   "yet_another", "test", "data", "value"};

  for (size_t i = 0; i < keys.size(); i++) {
    auto value = std::make_shared<int>(i * 100);
    map.Set(keys[i], std::move(value));
  }

  for (size_t i = 0; i < keys.size(); i++) {
    auto value = map.Get(keys[i]);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, static_cast<int>(i * 100));
  }

  for (const auto& key : keys) {
    map.Unset(key);
  }
}

TEST_F(ShardedMapTest, ConcurrentOperations) {
  ShardedMap<std::string, std::shared_ptr<int>> map;

  for (int i = 0; i < 50; i++) {
    std::string key = "key_" + std::to_string(i);
    auto value = std::make_shared<int>(i);
    map.Set(key, std::move(value));
  }

  std::vector<std::thread> threads;

  // Reader threads
  for (int t = 0; t < 5; t++) {
    threads.emplace_back([&map]() {
      for (int i = 0; i < 100; i++) {
        std::string key = "key_" + std::to_string(i % 50);
        auto val = map.Get(key);
        ASSERT_NE(val, nullptr);
      }
    });
  }

  // Writer threads
  for (int t = 0; t < 5; t++) {
    threads.emplace_back([&map, t]() {
      for (int i = 0; i < 20; i++) {
        std::string key = "new_key_" + std::to_string(t * 20 + i);
        auto value = std::make_shared<int>(1000 + t * 20 + i);
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
    auto value = map.Get(key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, i);
  }

  // Verify new data was written
  for (int t = 0; t < 5; t++) {
    for (int i = 0; i < 20; i++) {
      std::string key = "new_key_" + std::to_string(t * 20 + i);
      auto value = map.Get(key);
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
  ShardedMap<int, std::shared_ptr<int>> map;

  for (int i = 0; i < 20; i++) {
    auto value = std::make_shared<int>(i * 5);
    map.Set(i, std::move(value));
  }

  for (int i = 0; i < 20; i++) {
    auto value = map.Get(i);
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
