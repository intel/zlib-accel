// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "test_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "../config/config.h"

#ifdef DEBUG_LOG
void Log(std::string message) { std::cout << message << std::endl; }
#endif

static std::string GenerateRandomString(size_t length) {
  std::string random_string;
  for (unsigned int i = 0; i < length; i++) {
    char c = std::rand() % (std::numeric_limits<char>::max() -
                            std::numeric_limits<char>::min()) +
             std::numeric_limits<char>::min();
    random_string.push_back(c);
  }
  return random_string;
}

static char* GenerateCompressibleBlock(size_t length, int ratio = 4) {
  char* buf = new (std::nothrow) char[length];
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

static char* GenerateIncompressibleBlock(size_t length) {
  char* buf = new (std::nothrow) char[length];
  if (!buf) {
    return nullptr;
  }
  std::string random_string = GenerateRandomString(length);
  for (unsigned int i = 0; i < length; i++) {
    buf[i] = random_string[i];
  }
  return buf;
}

static char* GenerateZeroBlock(size_t length) {
  // The () is what calloc's zeroing becomes; without it the block is
  // uninitialized and nothing here would say so.
  char* buf = new (std::nothrow) char[length]();
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

static void GenerateSeededBytes(char* out, size_t length, uint32_t* state) {
  for (size_t i = 0; i < length; i++) {
    *state = (*state * 1103515245u) + 12345u;
    out[i] = static_cast<char>((*state >> 16) & 0xff);
  }
}

char* GenerateSeededCompressibleBlock(size_t length, uint32_t seed, int ratio) {
  char* buf = new (std::nothrow) char[length];
  if (!buf) {
    return nullptr;
  }

  const unsigned int compressible_string_length = 1024;
  unsigned int random_string_length = compressible_string_length / ratio;
  const unsigned int long_range = 8192;
  uint32_t state = seed;
  std::vector<char> string_long_range(random_string_length);
  GenerateSeededBytes(string_long_range.data(), random_string_length, &state);
  std::vector<char> string_short_range(random_string_length);
  unsigned int pos = 0;
  while (pos < length) {
    if (pos % compressible_string_length == 0) {
      GenerateSeededBytes(string_short_range.data(), random_string_length,
                          &state);
    }
    if ((pos % long_range) < random_string_length) {
      buf[pos] = string_long_range[pos % random_string_length];
    } else {
      buf[pos] = string_short_range[pos % random_string_length];
    }
    pos++;
  }

  return buf;
}

void DestroyBlock(char* buf) { delete[] buf; }

int ZlibCompress(const char* input, size_t input_length, std::string* output,
                 int window_bits, int flush, size_t* output_upper_bound,
                 ExecutionPath* execution_path) {
  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));

  int st =
      deflateInit2(&stream, -1, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
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

int ZlibUncompress(const char* input, size_t input_length, size_t output_length,
                   char** uncompressed, size_t* uncompressed_length,
                   size_t* input_consumed, int window_bits, int flush,
                   int input_chunks, ExecutionPath* execution_path) {
  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));

  // The caller owns a buffer on Z_STREAM_END and on the Z_OK partial return,
  // and nothing on any error, which leaves *uncompressed null rather than
  // handing back a buffer no caller checks the status before releasing.
  *uncompressed = nullptr;

  int st = inflateInit2(&stream, window_bits);
  if (st != Z_OK) {
    inflateEnd(&stream);
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
    *execution_path = GetInflateExecutionPath(&stream);

    // Z_OK on the last chunk means the input held less than a whole stream, so
    // the prefix that came back is the result the caller asked for rather than
    // a failure. Report its size and hand it over; the other two stop
    // conditions are errors and own nothing.
    bool partial_progress = (st == Z_OK && input_chunk == (input_chunks - 1));
    bool premature_end =
        (st == Z_STREAM_END && input_chunk < (input_chunks - 1));
    bool failed = (st != Z_OK && st != Z_STREAM_END);
    if (partial_progress || premature_end || failed) {
      if (partial_progress) {
        *uncompressed_length = stream.total_out;
        *input_consumed = stream.total_in;
      } else {
        delete[] *uncompressed;
        *uncompressed = nullptr;
      }
      inflateEnd(&stream);
      return st;
    }
  }
  *uncompressed_length = stream.total_out;
  *input_consumed = stream.total_in;

  inflateEnd(&stream);
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

void SetCompressPath(ExecutionPath path, bool zlib_fallback,
                     bool iaa_prepend_empty_block,
                     bool qat_compression_allow_chunking) {
  switch (path) {
    case ZLIB:
      config::SetConfig(config::USE_IAA_COMPRESS, 0);
      config::SetConfig(config::USE_IGZIP_COMPRESS, 0);
      config::SetConfig(config::USE_QAT_COMPRESS, 0);
      config::SetConfig(config::USE_ZLIB_COMPRESS, 1);
      break;
    case QAT:
      config::SetConfig(config::USE_IAA_COMPRESS, 0);
      config::SetConfig(config::USE_IGZIP_COMPRESS, 0);
      config::SetConfig(config::USE_QAT_COMPRESS, 1);
      config::SetConfig(config::USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IAA:
      config::SetConfig(config::USE_IAA_COMPRESS, 1);
      config::SetConfig(config::USE_IGZIP_COMPRESS, 0);
      config::SetConfig(config::USE_QAT_COMPRESS, 0);
      config::SetConfig(config::USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IGZIP:
      config::SetConfig(config::USE_IGZIP_COMPRESS, 1);
      config::SetConfig(config::USE_IAA_COMPRESS, 0);
      config::SetConfig(config::USE_QAT_COMPRESS, 0);
      config::SetConfig(config::USE_ZLIB_COMPRESS, zlib_fallback ? 1 : 0);
      break;
    default:
      break;
  }
  config::SetConfig(config::IAA_PREPEND_EMPTY_BLOCK, iaa_prepend_empty_block);
  config::SetConfig(config::QAT_COMPRESSION_ALLOW_CHUNKING,
                    qat_compression_allow_chunking);
}

void SetUncompressPath(ExecutionPath path, bool zlib_fallback,
                       bool iaa_prepend_empty_block) {
  switch (path) {
    case ZLIB:
      config::SetConfig(config::USE_IAA_UNCOMPRESS, 0);
      config::SetConfig(config::USE_IGZIP_UNCOMPRESS, 0);
      config::SetConfig(config::USE_QAT_UNCOMPRESS, 0);
      config::SetConfig(config::USE_ZLIB_UNCOMPRESS, 1);
      break;
    case QAT:
      config::SetConfig(config::USE_IAA_UNCOMPRESS, 0);
      config::SetConfig(config::USE_IGZIP_UNCOMPRESS, 0);
      config::SetConfig(config::USE_QAT_UNCOMPRESS, 1);
      config::SetConfig(config::USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IAA:
      config::SetConfig(config::USE_IAA_UNCOMPRESS, 1);
      config::SetConfig(config::USE_IGZIP_UNCOMPRESS, 0);
      config::SetConfig(config::USE_QAT_UNCOMPRESS, 0);
      config::SetConfig(config::USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
      break;
    case IGZIP:
      config::SetConfig(config::USE_IAA_UNCOMPRESS, 0);
      config::SetConfig(config::USE_IGZIP_UNCOMPRESS, 1);
      config::SetConfig(config::USE_QAT_UNCOMPRESS, 0);
      config::SetConfig(config::USE_ZLIB_UNCOMPRESS, zlib_fallback ? 1 : 0);
      break;
    default:
      break;
  }
  config::SetConfig(config::IAA_PREPEND_EMPTY_BLOCK, iaa_prepend_empty_block);
}
