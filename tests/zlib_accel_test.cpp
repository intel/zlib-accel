// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "../zlib_accel.h"

#include <gtest/gtest.h>
#include <stdio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <tuple>
#include <vector>

#include "../iaa.h"
#include "../logging.h"
#include "../qat.h"
#include "../utils.h"

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

int ZlibCompress(const char* input, size_t input_length, std::string* output,
                 int window_bits, int flush, size_t* output_upper_bound,
                 ExecutionPath* execution_path) {
  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));

  int st =
      deflateInit2(&stream, -1, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
  if (st != Z_OK) {
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
  *execution_path = zlib_accel_get_deflate_execution_path(&stream);
  if (st != Z_STREAM_END) {
    return st;
  }
  output->resize(stream.total_out);

  // TODO deflateEnd checks the internal status of the stream, which we cannot
  // set in the shim. Ignore return value for now.
  deflateEnd(&stream);
  return st;
}

int ZlibUncompress(const char* input, size_t input_length, size_t output_length,
                   char** uncompressed, size_t* uncompressed_length,
                   size_t* input_consumed, int window_bits, int flush,
                   int input_chunks, ExecutionPath* execution_path) {
  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));

  int st = inflateInit2(&stream, window_bits);
  if (st != Z_OK) {
    return st;
  }

  *uncompressed = new char[output_length];
  *uncompressed_length = 0;
  unsigned int input_chunk_size = input_length / input_chunks;
  for (int input_chunk = 0; input_chunk < input_chunks; input_chunk++) {
    unsigned int input_offset = input_chunk * input_chunk_size;
    unsigned int input_remaining = input_length - input_offset;
    if (input_chunk == (input_chunks - 1)) {
      input_chunk_size = input_remaining;
    }
    stream.next_in = (Bytef*)(input + input_offset);
    stream.avail_in = static_cast<unsigned int>(input_chunk_size);

    stream.next_out = (Bytef*)(*uncompressed + stream.total_out);
    stream.avail_out =
        static_cast<unsigned int>(output_length - stream.total_out);

    st = inflate(&stream, flush);
    *execution_path = zlib_accel_get_inflate_execution_path(&stream);
    if (st == Z_STREAM_END && input_chunk < (input_chunks - 1)) {
      return st;
    } else if (st == Z_OK && input_chunk == (input_chunks - 1)) {
      return st;
    } else if (st != Z_OK && st != Z_STREAM_END) {
      return st;
    }
  }
  *uncompressed_length = stream.total_out;
  *input_consumed = stream.total_in;

  inflateEnd(&stream);
  return st;
}

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
                     bool iaa_prepend_empty_block = false) {
  switch (path) {
    case ZLIB:
      zlib_accel_set_config(USE_IAA_COMPRESS, 0);
      zlib_accel_set_config(USE_QAT_COMPRESS, 0);
      zlib_accel_set_config(USE_ZLIB_COMPRESS, 1);
      break;
    case QAT:
      zlib_accel_set_config(USE_IAA_COMPRESS, 0);
      zlib_accel_set_config(USE_QAT_COMPRESS, 1);
      zlib_accel_set_config(USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IAA:
      zlib_accel_set_config(USE_IAA_COMPRESS, 1);
      zlib_accel_set_config(USE_QAT_COMPRESS, 0);
      zlib_accel_set_config(USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
  }
  zlib_accel_set_config(IAA_PREPEND_EMPTY_BLOCK, iaa_prepend_empty_block);
}

void SetUncompressPath(ExecutionPath path, bool zlib_fallback,
                       bool iaa_prepend_empty_block = false) {
  switch (path) {
    case ZLIB:
      zlib_accel_set_config(USE_IAA_UNCOMPRESS, 0);
      zlib_accel_set_config(USE_QAT_UNCOMPRESS, 0);
      zlib_accel_set_config(USE_ZLIB_UNCOMPRESS, 1);
      break;
    case QAT:
      zlib_accel_set_config(USE_IAA_UNCOMPRESS, 0);
      zlib_accel_set_config(USE_QAT_UNCOMPRESS, 1);
      zlib_accel_set_config(USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IAA:
      zlib_accel_set_config(USE_IAA_UNCOMPRESS, 1);
      zlib_accel_set_config(USE_QAT_UNCOMPRESS, 0);
      zlib_accel_set_config(USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
  }
  zlib_accel_set_config(IAA_PREPEND_EMPTY_BLOCK, iaa_prepend_empty_block);
}

struct TestParam {
  TestParam(ExecutionPath _execution_path_compress,
            bool _zlib_fallback_compress,
            ExecutionPath _execution_path_uncompress,
            bool _zlib_fallback_uncompress, int _window_bits_compress,
            int _flush_compress, int _window_bits_uncompress,
            int _flush_uncompress, int _input_chunks_uncompress,
            size_t _block_size, BlockCompressibilityType _block_type,
            bool _iaa_prepend_empty_block)
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
        iaa_prepend_empty_block(_iaa_prepend_empty_block) {}

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
};

bool ZlibCompressExpectFallback(TestParam test_param, size_t input_length,
                                size_t output_upper_bound) {
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
                                  bool compress_fallback = false) {
  bool fallback_expected = false;
#ifdef USE_QAT
  // if QAT selected, but options not supported or multi-call decompression, and
  // no zlib fallback
  if (test_param.execution_path_uncompress == QAT) {
    if (!SupportedOptionsQAT(
            window_bits_uncompress,
            compressed_length / test_param.input_chunks_uncompress)) {
      fallback_expected = true;
    } else if (input_length > QAT_HW_BUFF_SZ &&
               test_param.execution_path_compress != QAT) {
      // If it was not compressed by QAT, it is not chunked
      fallback_expected = true;
    } else if (input_length > QAT_HW_BUFF_SZ &&
               test_param.execution_path_compress == QAT &&
               ((GetCompressedFormat(window_bits_uncompress) ==
                     CompressedFormat::ZLIB &&
                 test_param.block_type == incompressible_block) ||
                GetCompressedFormat(window_bits_uncompress) ==
                    CompressedFormat::DEFLATE_RAW)) {
      // If data was compressed with QAT, it was chunked during compression
      // - gzip format: QAT decompression always possible (stream boundaries
      // detected before decompression)
      // - zlib format: QAT decompression possible if compressed
      // data fits in HW buffer size (it does not happen with incompressible
      // data).
      // - deflate raw: chunking during compression doesn't close the stream.
      // Decompression not possible.
      fallback_expected = true;
    } else if (test_param.input_chunks_uncompress > 1) {
      // Multi-chunk tests that were not skipped are expected to cause error
      fallback_expected = true;
    }
  }
#endif
#ifdef USE_IAA
  // if IAA selected, but options not supported or block not decompressible, and
  // no zlib fallback
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
    } else if (test_param.execution_path_compress == IAA && compress_fallback &&
               test_param.block_type == compressible_block) {
      // If IAA compression falls back to zlib (e.g., for 2MB blocks)
      // Incompressible or zero blocks don't need long-range references and can
      // still be decompressed
      fallback_expected = true;
    } else if (test_param.input_chunks_uncompress > 1) {
      // IAA with QPL_FLAG_LAST gets QPL_STS_BAD_EOF_ERR if a stream is not
      // decompressed in one call
      fallback_expected = true;
    }
  }
#endif
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

class ZlibTest
    : public testing::TestWithParam<
          std::tuple<ExecutionPath, bool, ExecutionPath, bool, int, int, int,
                     int, int, size_t, BlockCompressibilityType, bool>> {};

TEST_P(ZlibTest, CompressDecompress) {
  Log(LogLevel::LOG_INFO,
      testing::PrintToString(GetParam()).append("\n").c_str());

  TestParam test_param(std::get<0>(GetParam()), std::get<1>(GetParam()),
                       std::get<2>(GetParam()), std::get<3>(GetParam()),
                       std::get<4>(GetParam()), std::get<5>(GetParam()),
                       std::get<6>(GetParam()), std::get<7>(GetParam()),
                       std::get<8>(GetParam()), std::get<9>(GetParam()),
                       std::get<10>(GetParam()), std::get<11>(GetParam()));

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

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress,
                  test_param.iaa_prepend_empty_block);

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

  bool compress_fallback_expected =
      ZlibCompressExpectFallback(test_param, input_length, output_upper_bound);
  if (compress_fallback_expected && !test_param.zlib_fallback_compress) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    DestroyBlock(input);
    return;
  } else {
    ASSERT_EQ(ret, Z_STREAM_END);
    if (compress_fallback_expected) {
      ASSERT_EQ(execution_path, ZLIB);
    } else {
      ASSERT_EQ(execution_path, test_param.execution_path_compress);
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

  bool error_expected = false;
  bool uncompress_fallback_expected = ZlibUncompressExpectFallback(
      test_param, input_length, compressed, compressed.length(),
      window_bits_uncompress, compress_fallback_expected);
  if (uncompress_fallback_expected && !test_param.zlib_fallback_uncompress) {
    ASSERT_EQ(ret, Z_DATA_ERROR);
    error_expected = true;
  } else {
    ASSERT_EQ(ret, Z_STREAM_END);
    if (uncompress_fallback_expected) {
      ASSERT_EQ(execution_path, ZLIB);
    } else {
      ASSERT_EQ(execution_path, test_param.execution_path_uncompress);
    }
  }

  if (!error_expected) {
#ifdef USE_QAT
    if (test_param.execution_path_compress == QAT &&
        input_length > QAT_HW_BUFF_SZ &&
        GetCompressedFormat(window_bits_uncompress) !=
            CompressedFormat::DEFLATE_RAW) {
      // For data compressed by qzCompress, data is
      // made of multiple streams of hardware buffer size.
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

  delete[] uncompressed;
  DestroyBlock(input);
}

INSTANTIATE_TEST_SUITE_P(
    CompressDecompress, ZlibTest,
    testing::Combine(testing::Values(ZLIB
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
                     testing::Values(Z_PARTIAL_FLUSH, Z_SYNC_FLUSH),
                     testing::Values(1, 2),
                     testing::Values(1024, 4096, 16384, 262144, 2097152),
                     testing::Values(compressible_block, incompressible_block,
                                     zero_block),
                     testing::Values(false, true)));

class ZlibUtilityTest : public ZlibTest {};

TEST_P(ZlibUtilityTest, CompressDecompressUtility) {
  Log(LogLevel::LOG_INFO,
      testing::PrintToString(GetParam()).append("\n").c_str());

  TestParam test_param(std::get<0>(GetParam()), std::get<1>(GetParam()),
                       std::get<2>(GetParam()), std::get<3>(GetParam()),
                       std::get<4>(GetParam()), std::get<5>(GetParam()),
                       std::get<6>(GetParam()), std::get<7>(GetParam()),
                       std::get<8>(GetParam()), std::get<9>(GetParam()),
                       std::get<10>(GetParam()), std::get<11>(GetParam()));

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress);

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
                    test_param.zlib_fallback_uncompress);

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
    testing::Combine(testing::Values(ZLIB
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
                     testing::Values(compressible_block, incompressible_block,
                                     zero_block),
                     testing::Values(false)));

class ZlibUtility2Test : public ZlibTest {};

TEST_P(ZlibUtility2Test, CompressDecompressUtility2) {
  Log(LogLevel::LOG_INFO,
      testing::PrintToString(GetParam()).append("\n").c_str());

  TestParam test_param(std::get<0>(GetParam()), std::get<1>(GetParam()),
                       std::get<2>(GetParam()), std::get<3>(GetParam()),
                       std::get<4>(GetParam()), std::get<5>(GetParam()),
                       std::get<6>(GetParam()), std::get<7>(GetParam()),
                       std::get<8>(GetParam()), std::get<9>(GetParam()),
                       std::get<10>(GetParam()), std::get<11>(GetParam()));

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress);

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
                    test_param.zlib_fallback_uncompress);

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
    testing::Combine(testing::Values(ZLIB
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
                     testing::Values(compressible_block, incompressible_block,
                                     zero_block),
                     testing::Values(false)));

class ZlibPartialAndMultiStreamTest : public ZlibTest {};

TEST_P(ZlibPartialAndMultiStreamTest, CompressDecompressPartialStream) {
  Log(LogLevel::LOG_INFO,
      testing::PrintToString(GetParam()).append("\n").c_str());

  TestParam test_param(std::get<0>(GetParam()), std::get<1>(GetParam()),
                       std::get<2>(GetParam()), std::get<3>(GetParam()),
                       std::get<4>(GetParam()), std::get<5>(GetParam()),
                       std::get<6>(GetParam()), std::get<7>(GetParam()),
                       std::get<8>(GetParam()), std::get<9>(GetParam()),
                       std::get<10>(GetParam()), std::get<11>(GetParam()));

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress);

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
                    test_param.zlib_fallback_uncompress);

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
  Log(LogLevel::LOG_INFO,
      testing::PrintToString(GetParam()).append("\n").c_str());

  TestParam test_param(std::get<0>(GetParam()), std::get<1>(GetParam()),
                       std::get<2>(GetParam()), std::get<3>(GetParam()),
                       std::get<4>(GetParam()), std::get<5>(GetParam()),
                       std::get<6>(GetParam()), std::get<7>(GetParam()),
                       std::get<8>(GetParam()), std::get<9>(GetParam()),
                       std::get<10>(GetParam()), std::get<11>(GetParam()));

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress);

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
                    test_param.zlib_fallback_uncompress);

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
        // able to decompress if no references happend to be farther than 4kB.
        testing::Values(1024, 32768, 262144),
        testing::Values(compressible_block, incompressible_block),
        testing::Values(false)));

class ZlibGzipFileTest : public ZlibTest {};

TEST_P(ZlibGzipFileTest, CompressDecompressGzipFile) {
  Log(LogLevel::LOG_INFO,
      testing::PrintToString(GetParam()).append("\n").c_str());

  TestParam test_param(std::get<0>(GetParam()), std::get<1>(GetParam()),
                       std::get<2>(GetParam()), std::get<3>(GetParam()),
                       std::get<4>(GetParam()), std::get<5>(GetParam()),
                       std::get<6>(GetParam()), std::get<7>(GetParam()),
                       std::get<8>(GetParam()), std::get<9>(GetParam()),
                       std::get<10>(GetParam()), std::get<11>(GetParam()));

  SetCompressPath(test_param.execution_path_compress,
                  test_param.zlib_fallback_compress);

  size_t input_length = test_param.block_size;
  BlockCompressibilityType block_type = test_param.block_type;
  char* input = GenerateBlock(input_length, block_type);
  ASSERT_NE(input, nullptr);

  int ret = ZlibCompressGzipFile(input, input_length);
  ASSERT_EQ(ret, Z_OK);

  SetUncompressPath(test_param.execution_path_uncompress,
                    test_param.zlib_fallback_uncompress);

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
    testing::Combine(testing::Values(ZLIB
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
                     testing::Values(false)));

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
