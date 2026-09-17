// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// deflate() regression suites: the IGZIP deflate path and deflateParams.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "../config/config.h"
#include "../zlib_accel.h"
#include "test_utils.h"

using namespace config;

#ifdef USE_IGZIP
#include "../igzip.h"
#endif

#ifdef USE_IGZIP

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

#endif  // USE_IGZIP

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
