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

// Regression tests for the empty-flush rule on a mid-stream IGZIP deflate.
//
// ISA-L emits the sync marker for every SYNC_FLUSH and FULL_FLUSH whether or
// not one has just been emitted, so something has to refuse a flush that would
// only repeat the previous one -- otherwise a caller flushing on a timer with
// no new data grows the stream by marker bytes per tick, and the output stays
// valid deflate, so nothing downstream notices.  zlib's own rule is rank-based:
//
//   avail_in == 0 && flush != Z_FINISH && RANK(flush) <= RANK(last_flush)
//   RANK(f) = f * 2 - (f > 4 ? 9 : 0)
//   => Z_NO_FLUSH 0, Z_BLOCK 1, Z_PARTIAL_FLUSH 2, Z_TREES 3,
//      Z_SYNC_FLUSH 4, Z_FULL_FLUSH 6, Z_FINISH 8
//
// The rule this replaced asked instead whether ISA-L was already byte-aligned,
// which is a different question, and it read the flush value ISA-L had been
// given -- and igzip.cpp maps Z_SYNC_FLUSH, Z_PARTIAL_FLUSH and Z_BLOCK all
// onto ISA-L's SYNC_FLUSH.  So it fired on an empty Z_SYNC_FLUSH that outranked
// a preceding Z_PARTIAL_FLUSH, where zlib does work, and never fired on
// Z_FULL_FLUSH at all, which is the case that grows the stream.  Each test
// below names the term of the rule it covers.
class IGZIPEmptyFlushRegressionTest : public ::testing::Test {
 protected:
  static constexpr size_t kInputLength = 8 * 1024;

  void SetUp() override {
    saved_use_zlib_compress_ = GetConfig(USE_ZLIB_COMPRESS);
    saved_use_iaa_compress_ = GetConfig(USE_IAA_COMPRESS);
    saved_use_qat_compress_ = GetConfig(USE_QAT_COMPRESS);
    saved_use_igzip_compress_ = GetConfig(USE_IGZIP_COMPRESS);
    saved_use_zlib_uncompress_ = GetConfig(USE_ZLIB_UNCOMPRESS);
    saved_use_iaa_uncompress_ = GetConfig(USE_IAA_UNCOMPRESS);
    saved_use_qat_uncompress_ = GetConfig(USE_QAT_UNCOMPRESS);
    saved_use_igzip_uncompress_ = GetConfig(USE_IGZIP_UNCOMPRESS);
    // SetCompressPath/SetUncompressPath write these two unconditionally, so
    // they have to be restored as well or this fixture makes the suite
    // order-dependent.
    saved_iaa_prepend_empty_block_ = GetConfig(IAA_PREPEND_EMPTY_BLOCK);
    saved_qat_allow_chunking_ = GetConfig(QAT_COMPRESSION_ALLOW_CHUNKING);

    SetCompressPath(IGZIP, /*zlib_fallback=*/true, false, false);
    SetUncompressPath(ZLIB, false, false);

    input_ = GenerateSeededCompressibleBlock(kInputLength, /*seed=*/0x12a1);
    ASSERT_NE(input_, nullptr);
    memset(&stream_, 0, sizeof(stream_));
  }

  void TearDown() override {
    if (stream_open_) {
      deflateEnd(&stream_);
    }
    DestroyBlock(input_);
    SetConfig(USE_ZLIB_COMPRESS, saved_use_zlib_compress_);
    SetConfig(USE_IAA_COMPRESS, saved_use_iaa_compress_);
    SetConfig(USE_QAT_COMPRESS, saved_use_qat_compress_);
    SetConfig(USE_IGZIP_COMPRESS, saved_use_igzip_compress_);
    SetConfig(USE_ZLIB_UNCOMPRESS, saved_use_zlib_uncompress_);
    SetConfig(USE_IAA_UNCOMPRESS, saved_use_iaa_uncompress_);
    SetConfig(USE_QAT_UNCOMPRESS, saved_use_qat_uncompress_);
    SetConfig(USE_IGZIP_UNCOMPRESS, saved_use_igzip_uncompress_);
    SetConfig(IAA_PREPEND_EMPTY_BLOCK, saved_iaa_prepend_empty_block_);
    SetConfig(QAT_COMPRESSION_ALLOW_CHUNKING, saved_qat_allow_chunking_);
  }

  // Opens a deflate stream and hands the whole payload to one call with
  // `first_flush`, which becomes the last_flush the empty calls are ranked
  // against.  Output is accumulated so a test can inflate what the stream
  // produced.
  void StartStream(int window_bits, int first_flush) {
    ASSERT_EQ(deflateInit2(&stream_, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           window_bits, 8, Z_DEFAULT_STRATEGY),
              Z_OK);
    stream_open_ = true;
    compressed_.assign(kInputLength * 2 + 4096, 0);
    compressed_used_ = 0;

    stream_.next_in = reinterpret_cast<Bytef*>(input_);
    stream_.avail_in = static_cast<unsigned int>(kInputLength);
    stream_.next_out = reinterpret_cast<Bytef*>(compressed_.data());
    stream_.avail_out = static_cast<unsigned int>(compressed_.size());
    const int ret = deflate(&stream_, first_flush);
    ASSERT_TRUE(ret == Z_OK || ret == Z_BUF_ERROR) << "ret=" << ret;
    ASSERT_EQ(stream_.avail_in, 0u);
    ASSERT_EQ(GetDeflateExecutionPath(&stream_), IGZIP);
    compressed_used_ = compressed_.size() - stream_.avail_out;
  }

  // One deflate() call carrying no input at all, appending whatever it
  // produces. Returns the return code; `bytes` receives the byte count.
  int EmptyFlush(int flush, size_t* bytes) {
    stream_.next_in = nullptr;
    stream_.avail_in = 0;
    stream_.next_out =
        reinterpret_cast<Bytef*>(compressed_.data()) + compressed_used_;
    stream_.avail_out =
        static_cast<unsigned int>(compressed_.size() - compressed_used_);
    const unsigned int before = stream_.avail_out;
    const int ret = deflate(&stream_, flush);
    *bytes = before - stream_.avail_out;
    compressed_used_ += *bytes;
    return ret;
  }

  // Inflates everything the stream has emitted so far and compares it with the
  // payload.  A refusal must not cost the caller bytes it already handed over,
  // and a flush that does run must leave the stream decodable at that point.
  void ExpectPayloadRecoverable(int window_bits) {
    z_stream inflate_stream;
    memset(&inflate_stream, 0, sizeof(inflate_stream));
    ASSERT_EQ(inflateInit2(&inflate_stream, window_bits), Z_OK);
    std::vector<char> decompressed(kInputLength + 4096, 0);
    inflate_stream.next_in = reinterpret_cast<Bytef*>(compressed_.data());
    inflate_stream.avail_in = static_cast<unsigned int>(compressed_used_);
    inflate_stream.next_out = reinterpret_cast<Bytef*>(decompressed.data());
    inflate_stream.avail_out = static_cast<unsigned int>(decompressed.size());
    const int ret = inflate(&inflate_stream, Z_SYNC_FLUSH);
    EXPECT_TRUE(ret == Z_OK || ret == Z_BUF_ERROR || ret == Z_STREAM_END)
        << "ret=" << ret;
    EXPECT_EQ(inflate_stream.total_out, kInputLength);
    EXPECT_EQ(memcmp(decompressed.data(), input_, kInputLength), 0);
    inflateEnd(&inflate_stream);
  }

  z_stream stream_;
  bool stream_open_ = false;
  char* input_ = nullptr;
  std::vector<char> compressed_;
  size_t compressed_used_ = 0;

 private:
  uint32_t saved_use_zlib_compress_ = 0;
  uint32_t saved_use_iaa_compress_ = 0;
  uint32_t saved_use_qat_compress_ = 0;
  uint32_t saved_use_igzip_compress_ = 0;
  uint32_t saved_use_zlib_uncompress_ = 0;
  uint32_t saved_use_iaa_uncompress_ = 0;
  uint32_t saved_use_qat_uncompress_ = 0;
  uint32_t saved_use_igzip_uncompress_ = 0;
  uint32_t saved_iaa_prepend_empty_block_ = 0;
  uint32_t saved_qat_allow_chunking_ = 0;
};

// The rank term, in the direction the alignment rule got wrong: an empty flush
// that outranks its predecessor does work.  Z_PARTIAL_FLUSH (2) outranks
// Z_NO_FLUSH (0), and Z_SYNC_FLUSH (4) outranks Z_PARTIAL_FLUSH -- and because
// igzip.cpp maps both onto ISA-L's SYNC_FLUSH, the alignment rule refused the
// second of these with zero bytes.
TEST_F(IGZIPEmptyFlushRegressionTest, EmptyFlushThatOutranksPredecessorRuns) {
  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_NO_FLUSH));

  size_t partial_bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_PARTIAL_FLUSH, &partial_bytes), Z_OK);
  EXPECT_GT(partial_bytes, 0u);
  EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP);

  size_t sync_bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_SYNC_FLUSH, &sync_bytes), Z_OK);
  EXPECT_GT(sync_bytes, 0u);
  EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP);

  ExpectPayloadRecoverable(-15);
}

// The ranking is not the numeric order of the flush constants.  Z_BLOCK was
// added after the others and its value, 5, is above Z_FINISH's 4, but it ranks
// between Z_NO_FLUSH and Z_PARTIAL_FLUSH -- so an empty Z_BLOCK after a
// Z_SYNC_FLUSH is redundant, while after a Z_NO_FLUSH it does work.  Comparing
// the constants directly gets both of these backwards.
TEST_F(IGZIPEmptyFlushRegressionTest, ZBlockRanksBelowSyncFlushNotAboveFinish) {
  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_SYNC_FLUSH));

  size_t bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_BLOCK, &bytes), Z_BUF_ERROR);
  EXPECT_EQ(bytes, 0u);
  ExpectPayloadRecoverable(-15);

  ASSERT_EQ(deflateEnd(&stream_), Z_OK);
  stream_open_ = false;
  memset(&stream_, 0, sizeof(stream_));

  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_NO_FLUSH));
  EXPECT_EQ(EmptyFlush(Z_BLOCK, &bytes), Z_OK);
  EXPECT_GT(bytes, 0u);
  ExpectPayloadRecoverable(-15);
}

// The same rank term in the refusing direction, on the flush value the
// alignment rule never tested: a repeated empty Z_FULL_FLUSH must be refused
// with zero bytes, however many times it is asked for.  Unrefused, each call
// appended another marker and the stream grew without bound.
TEST_F(IGZIPEmptyFlushRegressionTest, RepeatedEmptyFullFlushProducesNoBytes) {
  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_FULL_FLUSH));
  const size_t after_first_flush = compressed_used_;

  for (int iter = 0; iter < 8; ++iter) {
    size_t bytes = 0;
    EXPECT_EQ(EmptyFlush(Z_FULL_FLUSH, &bytes), Z_BUF_ERROR) << "iter=" << iter;
    EXPECT_EQ(bytes, 0u) << "iter=" << iter;
    EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP) << "iter=" << iter;
  }
  EXPECT_EQ(compressed_used_, after_first_flush);

  ExpectPayloadRecoverable(-15);
}

// The record-before-deciding term.  zlib stores the flush of every call in
// last_flush, including one it goes on to refuse, so a refused low-rank flush
// *lowers* last_flush and a higher-ranked flush after it does work.  Recording
// only the calls that ran would leave last_flush at Z_FULL_FLUSH here and
// refuse the Z_SYNC_FLUSH too.
TEST_F(IGZIPEmptyFlushRegressionTest, RefusedFlushStillLowersTheBar) {
  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_FULL_FLUSH));

  size_t partial_bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_PARTIAL_FLUSH, &partial_bytes), Z_BUF_ERROR);
  EXPECT_EQ(partial_bytes, 0u);

  size_t sync_bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_SYNC_FLUSH, &sync_bytes), Z_OK);
  EXPECT_GT(sync_bytes, 0u);

  ExpectPayloadRecoverable(-15);
}

// The Z_FINISH exemption: Z_FINISH is never refused as redundant, whatever the
// previous flush was, because it is how a stream ends.
TEST_F(IGZIPEmptyFlushRegressionTest, EmptyFinishAfterFullFlushStillEnds) {
  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_FULL_FLUSH));

  size_t bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_FINISH, &bytes), Z_STREAM_END);
  EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP);

  ExpectPayloadRecoverable(-15);
}

// The out-of-output-space term.  A caller draining one flush through a small
// output buffer repeats the same flush value with no new input, which is
// exactly the shape the rank rule refuses; zlib serves it anyway because a call
// that ran out of room puts last_flush back below every flush value.  Refusing
// it would strand the caller in a Z_BUF_ERROR loop with the flush half written.
TEST_F(IGZIPEmptyFlushRegressionTest, FlushDrainsThroughASmallOutputBuffer) {
  for (const unsigned int chunk : {7u, 8u, 16u, 64u}) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                           Z_DEFAULT_STRATEGY),
              Z_OK)
        << "chunk=" << chunk;

    std::vector<char> compressed(kInputLength * 2 + 4096, 0);
    size_t used = 0;
    stream.next_in = reinterpret_cast<Bytef*>(input_);
    stream.avail_in = static_cast<unsigned int>(kInputLength);

    int ret = Z_OK;
    int calls = 0;
    for (; calls < 100000; ++calls) {
      if (used + chunk > compressed.size()) {
        break;
      }
      stream.next_out = reinterpret_cast<Bytef*>(compressed.data()) + used;
      stream.avail_out = chunk;
      ret = deflate(&stream, Z_SYNC_FLUSH);
      used += chunk - stream.avail_out;
      if (ret != Z_OK) {
        break;
      }
      // The flush is drained once a call leaves room unused with no input
      // left: nothing is pending on either side.
      if (stream.avail_out != 0 && stream.avail_in == 0) {
        ++calls;
        break;
      }
      stream.next_in = nullptr;
      stream.avail_in = 0;
    }
    EXPECT_EQ(ret, Z_OK) << "chunk=" << chunk;
    EXPECT_LT(calls, 100000) << "chunk=" << chunk;

    // Only now is the flush genuinely redundant, and only now may it be
    // refused.
    stream.next_in = nullptr;
    stream.avail_in = 0;
    stream.next_out = reinterpret_cast<Bytef*>(compressed.data()) + used;
    stream.avail_out = 64;
    EXPECT_EQ(deflate(&stream, Z_SYNC_FLUSH), Z_BUF_ERROR) << "chunk=" << chunk;
    EXPECT_EQ(stream.avail_out, 64u) << "chunk=" << chunk;
    deflateEnd(&stream);

    z_stream inflate_stream;
    memset(&inflate_stream, 0, sizeof(inflate_stream));
    ASSERT_EQ(inflateInit2(&inflate_stream, -15), Z_OK) << "chunk=" << chunk;
    std::vector<char> decompressed(kInputLength + 4096, 0);
    inflate_stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
    inflate_stream.avail_in = static_cast<unsigned int>(used);
    inflate_stream.next_out = reinterpret_cast<Bytef*>(decompressed.data());
    inflate_stream.avail_out = static_cast<unsigned int>(decompressed.size());
    inflate(&inflate_stream, Z_SYNC_FLUSH);
    EXPECT_EQ(inflate_stream.total_out, kInputLength) << "chunk=" << chunk;
    EXPECT_EQ(memcmp(decompressed.data(), input_, kInputLength), 0)
        << "chunk=" << chunk;
    inflateEnd(&inflate_stream);
  }
}

// deflateReset must clear the recorded flush, since the restarted stream has
// emitted nothing that a flush could repeat.  Leaving it set refuses the first
// flush of every stream after the first.
TEST_F(IGZIPEmptyFlushRegressionTest, ResetClearsTheRecordedFlush) {
  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_FULL_FLUSH));

  size_t bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_FULL_FLUSH, &bytes), Z_BUF_ERROR);
  EXPECT_EQ(bytes, 0u);

  ASSERT_EQ(deflateReset(&stream_), Z_OK);
  compressed_used_ = 0;

  // The restarted stream has emitted nothing, so this Z_FULL_FLUSH is not the
  // one that was just refused: it must do work, exactly as it would on a stream
  // that had only just been opened.  It has to be the *first* call after the
  // reset -- any input-bearing call in between would record its own flush and
  // hide a record the reset failed to clear.
  EXPECT_EQ(EmptyFlush(Z_FULL_FLUSH, &bytes), Z_OK);
  EXPECT_GT(bytes, 0u);

  stream_.next_in = reinterpret_cast<Bytef*>(input_);
  stream_.avail_in = static_cast<unsigned int>(kInputLength);
  stream_.next_out =
      reinterpret_cast<Bytef*>(compressed_.data()) + compressed_used_;
  stream_.avail_out =
      static_cast<unsigned int>(compressed_.size() - compressed_used_);
  EXPECT_EQ(deflate(&stream_, Z_FINISH), Z_STREAM_END);
  compressed_used_ = compressed_.size() - stream_.avail_out;

  ExpectPayloadRecoverable(-15);
}

// The "no input" term.  A call that carries input is never refused, however
// redundant its flush value looks -- it has something to compress.
TEST_F(IGZIPEmptyFlushRegressionTest, InputBearingRepeatedFlushIsNeverRefused) {
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  std::vector<char> compressed(kInputLength * 2 + 4096, 0);
  size_t used = 0;

  for (int half = 0; half < 2; ++half) {
    stream.next_in =
        reinterpret_cast<Bytef*>(input_) + half * (kInputLength / 2);
    stream.avail_in = static_cast<unsigned int>(kInputLength / 2);
    stream.next_out = reinterpret_cast<Bytef*>(compressed.data()) + used;
    stream.avail_out = static_cast<unsigned int>(compressed.size() - used);
    const unsigned int before = stream.avail_out;
    EXPECT_EQ(deflate(&stream, Z_FULL_FLUSH), Z_OK) << "half=" << half;
    EXPECT_EQ(stream.avail_in, 0u) << "half=" << half;
    EXPECT_GT(before - stream.avail_out, 0u) << "half=" << half;
    EXPECT_EQ(GetDeflateExecutionPath(&stream), IGZIP) << "half=" << half;
    used += before - stream.avail_out;
  }
  deflateEnd(&stream);

  z_stream inflate_stream;
  memset(&inflate_stream, 0, sizeof(inflate_stream));
  ASSERT_EQ(inflateInit2(&inflate_stream, -15), Z_OK);
  std::vector<char> decompressed(kInputLength + 4096, 0);
  inflate_stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  inflate_stream.avail_in = static_cast<unsigned int>(used);
  inflate_stream.next_out = reinterpret_cast<Bytef*>(decompressed.data());
  inflate_stream.avail_out = static_cast<unsigned int>(decompressed.size());
  inflate(&inflate_stream, Z_SYNC_FLUSH);
  EXPECT_EQ(inflate_stream.total_out, kInputLength);
  EXPECT_EQ(memcmp(decompressed.data(), input_, kInputLength), 0);
  inflateEnd(&inflate_stream);
}

// An empty flush that is *not* redundant reports Z_OK even when it finds
// nothing to do.  zlib seeds its last_flush below every flush value, so the
// opening call on a stream is never the refused one however empty it is; only
// the call after it can be.
TEST_F(IGZIPEmptyFlushRegressionTest, EmptyOpeningFlushIsServedNotRefused) {
  ASSERT_EQ(deflateInit2(&stream_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  stream_open_ = true;
  compressed_.assign(4096, 0);
  compressed_used_ = 0;

  size_t bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_NO_FLUSH, &bytes), Z_OK);
  EXPECT_EQ(bytes, 0u);
  EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP);

  EXPECT_EQ(EmptyFlush(Z_NO_FLUSH, &bytes), Z_BUF_ERROR);
  EXPECT_EQ(bytes, 0u);
}

// A call with no output space is refused before the flush value is even looked
// at, which is both what zlib returns and what keeps the flush unrecorded: the
// caller was denied it, so a later call asking for the same flush still has
// work to do.  Running ISA-L anyway also consumed input on a call zlib leaves
// the caller holding.
TEST_F(IGZIPEmptyFlushRegressionTest, NoOutputSpaceIsRefusedWithoutRecording) {
  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_SYNC_FLUSH));
  const size_t after_first_flush = compressed_used_;

  // Denied, carrying input: the input must still be the caller's afterwards.
  stream_.next_in = reinterpret_cast<Bytef*>(input_);
  stream_.avail_in = static_cast<unsigned int>(kInputLength);
  stream_.next_out =
      reinterpret_cast<Bytef*>(compressed_.data()) + compressed_used_;
  stream_.avail_out = 0;
  EXPECT_EQ(deflate(&stream_, Z_FULL_FLUSH), Z_BUF_ERROR);
  EXPECT_EQ(stream_.avail_in, kInputLength);
  EXPECT_EQ(compressed_used_, after_first_flush);

  // Denied, empty.
  size_t bytes = 0;
  stream_.avail_out = 0;
  stream_.next_in = nullptr;
  stream_.avail_in = 0;
  EXPECT_EQ(deflate(&stream_, Z_FULL_FLUSH), Z_BUF_ERROR);

  // The stream's recorded flush is still the Z_SYNC_FLUSH of the opening call,
  // so an empty Z_PARTIAL_FLUSH is still redundant.  Treating "no room at all"
  // as "ran out of room part-way" would put the record below every flush value
  // and serve this.
  EXPECT_EQ(EmptyFlush(Z_PARTIAL_FLUSH, &bytes), Z_BUF_ERROR);
  EXPECT_EQ(bytes, 0u);

  // The Z_FULL_FLUSH the two denied calls asked for has not been performed, so
  // this one is not redundant and must do work.  Recording a denied call's
  // flush would refuse it.
  EXPECT_EQ(EmptyFlush(Z_FULL_FLUSH, &bytes), Z_OK);
  EXPECT_GT(bytes, 0u);

  // And now it is redundant.
  EXPECT_EQ(EmptyFlush(Z_FULL_FLUSH, &bytes), Z_BUF_ERROR);
  EXPECT_EQ(bytes, 0u);

  ExpectPayloadRecoverable(-15);
}

// The rule is zlib's, not a raw-deflate quirk: the wrapped formats answer the
// same way.  A zlib or gzip header is emitted by the first call, so it cannot
// be what makes the second one do work.
TEST_F(IGZIPEmptyFlushRegressionTest, WrappedFormatsFollowTheSameRule) {
  for (const int window_bits : {15, 31}) {
    memset(&stream_, 0, sizeof(stream_));
    ASSERT_NO_FATAL_FAILURE(StartStream(window_bits, Z_SYNC_FLUSH));

    size_t bytes = 0;
    EXPECT_EQ(EmptyFlush(Z_SYNC_FLUSH, &bytes), Z_BUF_ERROR)
        << "window_bits=" << window_bits;
    EXPECT_EQ(bytes, 0u) << "window_bits=" << window_bits;

    EXPECT_EQ(EmptyFlush(Z_FULL_FLUSH, &bytes), Z_OK)
        << "window_bits=" << window_bits;
    EXPECT_GT(bytes, 0u) << "window_bits=" << window_bits;

    ExpectPayloadRecoverable(window_bits);

    ASSERT_EQ(deflateEnd(&stream_), Z_OK) << "window_bits=" << window_bits;
    stream_open_ = false;
  }
}

// zlib checks the flush range before it looks at anything else and rejects an
// out-of-range value without touching the stream, so an accelerated stream has
// to come out of such a call exactly as it went in.  Both halves of that were
// wrong: the value was recorded as the stream's last flush, where it outranks
// every legal one and refuses the caller's next empty flush, and since no
// engine accepts it the call landed on the zlib fall-through, which pinned a
// stream ISA-L was still holding to ZLIB -- so the buffered payload was never
// emitted and the caller's own Z_FINISH produced a valid, empty stream.
TEST_F(IGZIPEmptyFlushRegressionTest, InvalidFlushLeavesTheStreamUntouched) {
  ASSERT_NO_FATAL_FAILURE(StartStream(-15, Z_NO_FLUSH));

  // Z_TREES is one above deflate's Z_BLOCK and a legal flush for inflate, which
  // makes it the out-of-range value a caller is likeliest to pass by mistake.
  for (const int flush : {Z_TREES, 99, -1}) {
    size_t bytes = 0;
    EXPECT_EQ(EmptyFlush(flush, &bytes), Z_STREAM_ERROR) << "flush=" << flush;
    EXPECT_EQ(bytes, 0u) << "flush=" << flush;
    EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP) << "flush=" << flush;
  }

  // Having no output space does not change the answer, the way it does for a
  // legal flush: zlib's range check comes first.
  stream_.next_in = nullptr;
  stream_.avail_in = 0;
  stream_.next_out =
      reinterpret_cast<Bytef*>(compressed_.data()) + compressed_used_;
  stream_.avail_out = 0;
  EXPECT_EQ(deflate(&stream_, 99), Z_STREAM_ERROR);
  EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP);

  // The recorded flush is still the opening Z_NO_FLUSH, so an empty
  // Z_PARTIAL_FLUSH outranks it and does work -- and that is also the call that
  // proves the payload is still ISA-L's to emit.
  size_t bytes = 0;
  EXPECT_EQ(EmptyFlush(Z_PARTIAL_FLUSH, &bytes), Z_OK);
  EXPECT_GT(bytes, 0u);
  EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP);

  ExpectPayloadRecoverable(-15);
}

// The same value on the first call, where there is no engine holding anything
// yet: the cost then is the pin itself, which takes the stream off the offload
// for the rest of its life over a call zlib treats as a no-op.
TEST_F(IGZIPEmptyFlushRegressionTest,
       InvalidFlushOnAFreshStreamKeepsItOffloadable) {
  ASSERT_EQ(deflateInit2(&stream_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  stream_open_ = true;
  compressed_.assign(kInputLength * 2 + 4096, 0);
  compressed_used_ = 0;

  size_t bytes = 0;
  EXPECT_EQ(EmptyFlush(99, &bytes), Z_STREAM_ERROR);
  EXPECT_EQ(bytes, 0u);
  EXPECT_EQ(GetDeflateExecutionPath(&stream_), UNDEFINED);

  stream_.next_in = reinterpret_cast<Bytef*>(input_);
  stream_.avail_in = static_cast<unsigned int>(kInputLength);
  stream_.next_out = reinterpret_cast<Bytef*>(compressed_.data());
  stream_.avail_out = static_cast<unsigned int>(compressed_.size());
  EXPECT_EQ(deflate(&stream_, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(GetDeflateExecutionPath(&stream_), IGZIP);
  compressed_used_ = compressed_.size() - stream_.avail_out;

  ExpectPayloadRecoverable(-15);
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
  // The documented consequence of staying on IGZIP: the level the call asked
  // for is not honored, so the half that was supposed to be stored is
  // compressed like the first half.  Stored blocks cannot come out smaller than
  // the data they store, so a total below the input length proves the request
  // was not honored -- which is the residual the README describes, asserted
  // here so a change in it cannot pass unnoticed.
  EXPECT_LT(produced, input_length);
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
