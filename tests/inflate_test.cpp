// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// inflate() regression suites: the IGZIP inflate path, the accelerator ->
// IGZIP fallbacks, the flush/data_type gate, the dictionary fallback and the
// remembered IAA history-window rejection.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../config/config.h"
#include "../iaa.h"
#include "../zlib_accel.h"
#include "test_utils.h"

using namespace config;

#ifdef USE_IGZIP
#include "../igzip.h"
#endif

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
  EXPECT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(stream.avail_in, 1u);

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
  EXPECT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP);

  // The stream ends inside its last byte, and the byte after it belongs to the
  // caller: exactly one byte must come back unconsumed. "No more than one" also
  // passed when the decompressor swallowed it.
  EXPECT_EQ(stream.avail_in, 1u);

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
    // The byte is past the end of the stream, so no call may take it, whether
    // the stream has already ended or is still handing back buffered output.
    // ISA-L takes the whole input into its own buffer long before it has
    // delivered the payload, so unlike zlib it is usually still draining here.
    EXPECT_EQ(stream.avail_in, 1u) << "input_length=" << input_length;
    EXPECT_TRUE(one_byte_ret == Z_OK || one_byte_ret == Z_STREAM_END)
        << "ret=" << one_byte_ret << " input_length=" << input_length;

    // Finish the drain with the byte still on offer: the stream must end, hand
    // back exactly the payload, and leave the byte where it found it.
    for (int iter = 0; iter < 4096 && one_byte_ret != Z_STREAM_END; ++iter) {
      stream.next_out = reinterpret_cast<Bytef*>(output_chunk.data());
      stream.avail_out = static_cast<unsigned int>(output_chunk.size());
      one_byte_ret = inflate(&stream, Z_SYNC_FLUSH);
      ASSERT_TRUE(one_byte_ret == Z_OK || one_byte_ret == Z_STREAM_END)
          << "ret=" << one_byte_ret << " input_length=" << input_length;
      ASSERT_EQ(stream.avail_in, 1u) << "input_length=" << input_length;
    }
    EXPECT_EQ(one_byte_ret, Z_STREAM_END) << "input_length=" << input_length;
    EXPECT_EQ(stream.total_out, input_length)
        << "input_length=" << input_length;

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
  // Both of these were once permissive -- any path, and any return but
  // Z_DATA_ERROR -- which let the test pass without IGZIP ever running, and
  // without the stream having ended where it should.
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);
  ASSERT_EQ(ret, Z_STREAM_END);

  // No output is expected for empty payload. Most importantly, all trailing
  // bytes must remain unconsumed for the caller.
  EXPECT_EQ(output.size() - stream.avail_out, 0u);
  EXPECT_EQ(stream.avail_in, trailing_len)
      << "compressed_size=" << compressed.size();

  inflateEnd(&stream);
}

// The end of a raw deflate stream almost never lands on a byte boundary, so the
// decompressor has to stop at the right *bit* and hand the rest of the byte's
// container back to the caller as a whole byte.  Getting that wrong shows up as
// an off-by-one in avail_in, which the tests above tolerated: one permitted the
// ZLIB path, another asserted only that avail_in had not grown.  This pins the
// byte count exactly, on payloads chosen to leave every possible number of
// spare bits in the final byte.
//
// The spare-bit count is not observable from a finished stream -- zlib drops
// the remainder of the last byte before it returns Z_STREAM_END -- so the
// boundary is *constructed* instead.  A static-Huffman block has an exactly
// predictable bit length: 3 for the block header, 8 bits per literal below 144,
// 9 bits per literal at or above it, and 7 for end-of-block.  So k literals >=
// 144 put the stream's last bit at 8 * (m + k) + k + 10, and k = 0..7 walks all
// eight boundaries.  Z_FIXED asks for the static block, and every byte of the
// payload is distinct so the encoder finds no match to spend different bits on.
TEST(IGZIPInflateRegressionTest, RawStreamEndsOnEverySubByteBoundary) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, false, false);

  constexpr size_t kTrailingLen = 8;
  const char kTrailing[kTrailingLen + 1] = "SENTINEL";
  constexpr int kLowLiterals = 24;

  for (int k = 0; k < 8; ++k) {
    const int expected_spare_bits = (8 - (k + 2) % 8) % 8;
    SCOPED_TRACE("k=" + std::to_string(k) +
                 " spare_bits=" + std::to_string(expected_spare_bits));

    std::string payload;
    for (int i = 0; i < k; ++i) {
      payload.push_back(static_cast<char>(200 + i));
    }
    for (int i = 0; i < kLowLiterals; ++i) {
      payload.push_back(static_cast<char>(1 + i));
    }

    z_stream cstream;
    memset(&cstream, 0, sizeof(z_stream));
    ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                           Z_FIXED),
              Z_OK);
    std::vector<char> compressed_buffer(payload.size() + 64);
    cstream.next_in = reinterpret_cast<Bytef*>(payload.data());
    cstream.avail_in = static_cast<unsigned int>(payload.size());
    cstream.next_out = reinterpret_cast<Bytef*>(compressed_buffer.data());
    cstream.avail_out = static_cast<unsigned int>(compressed_buffer.size());
    ASSERT_EQ(deflate(&cstream, Z_FINISH), Z_STREAM_END);
    const std::string compressed(compressed_buffer.data(), cstream.total_out);
    ASSERT_EQ(deflateEnd(&cstream), Z_OK);

    // If this fails the encoder did not emit the block this test assumes -- a
    // stored block, or a match -- and the boundary is not the one named above.
    const size_t bits = 3 + 8 * kLowLiterals + 9 * k + 7;
    ASSERT_EQ(compressed.size(), (bits + 7) / 8);
    ASSERT_EQ(8 * compressed.size() - bits,
              static_cast<size_t>(expected_spare_bits));

    std::string with_trailing = compressed;
    with_trailing.append(kTrailing, kTrailingLen);

    z_stream stream;
    memset(&stream, 0, sizeof(z_stream));
    ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);
    std::vector<char> output(payload.size() + 64);
    stream.next_in = reinterpret_cast<Bytef*>(with_trailing.data());
    stream.avail_in = static_cast<unsigned int>(with_trailing.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());

    const int ret = inflate(&stream, Z_SYNC_FLUSH);
    EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP);
    EXPECT_EQ(ret, Z_STREAM_END);
    EXPECT_EQ(stream.avail_in, kTrailingLen);
    EXPECT_EQ(stream.total_out, payload.size());
    EXPECT_EQ(memcmp(output.data(), payload.data(), payload.size()), 0);
    if (stream.avail_in == kTrailingLen) {
      EXPECT_EQ(memcmp(stream.next_in, kTrailing, kTrailingLen), 0);
    }
    inflateEnd(&stream);
  }
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

// zlib documents data_type as being written every time inflate() returns, under
// every flush value.  An offloaded call cannot compute it -- ISA-L reaches its
// block-header states inside a single isal_inflate() call and reports no bit
// position -- so the field is deliberately left as the caller left it rather
// than guessed at, which the README lists as a known divergence.  Left
// unasserted, a later change could start writing a plausible-looking but wrong
// value, which no caller could tell from a right one.  The sentinel is a value
// zlib would never leave behind: bit 6 and above are the block-boundary flags,
// and 63 is outside the 0-7 bit position zlib reports.
TEST(IGZIPInflateRegressionTest, InflateLeavesDataTypeUntouched) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const int kSentinel = 0x5A5A;
  const size_t input_length = 16 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, 0x1d7a);
  ASSERT_NE(input, nullptr);

  for (int window_bits : {-15, 15, 31}) {
    std::string compressed;
    size_t output_upper_bound;
    ExecutionPath compress_path = UNDEFINED;
    ASSERT_EQ(ZlibCompress(input, input_length, &compressed, window_bits,
                           Z_FINISH, &output_upper_bound, &compress_path),
              Z_STREAM_END)
        << "window_bits=" << window_bits;

    z_stream stream;
    memset(&stream, 0, sizeof(z_stream));
    ASSERT_EQ(inflateInit2(&stream, window_bits), Z_OK);
    stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());

    // Small chunks, so the payload takes several calls and the flush value
    // varies across them.
    std::vector<char> chunk(1024);
    const int flushes[] = {Z_NO_FLUSH, Z_SYNC_FLUSH, Z_PARTIAL_FLUSH, Z_FINISH};
    int ret = Z_OK;
    size_t recovered = 0;
    for (int call = 0; call < 4096 && ret != Z_STREAM_END; ++call) {
      stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
      stream.avail_out = static_cast<uInt>(chunk.size());
      stream.data_type = kSentinel;
      ret = inflate(&stream, flushes[call % 4]);
      ASSERT_TRUE(ret == Z_OK || ret == Z_STREAM_END || ret == Z_BUF_ERROR)
          << "window_bits=" << window_bits << " call=" << call
          << " ret=" << ret;
      ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP)
          << "window_bits=" << window_bits;
      ASSERT_EQ(stream.data_type, kSentinel)
          << "window_bits=" << window_bits << " call=" << call;
      const size_t produced = chunk.size() - stream.avail_out;
      ASSERT_LE(recovered + produced, input_length);
      EXPECT_EQ(memcmp(chunk.data(), input + recovered, produced), 0)
          << "window_bits=" << window_bits << " call=" << call;
      recovered += produced;
      if (ret == Z_BUF_ERROR) {
        break;
      }
    }
    EXPECT_EQ(ret, Z_STREAM_END) << "window_bits=" << window_bits;
    EXPECT_EQ(recovered, input_length) << "window_bits=" << window_bits;

    // The terminal-state gate answers the next call, and leaves the field alone
    // for the same reason.
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    stream.data_type = kSentinel;
    EXPECT_EQ(inflate(&stream, Z_NO_FLUSH), Z_STREAM_END)
        << "window_bits=" << window_bits;
    EXPECT_EQ(stream.data_type, kSentinel) << "window_bits=" << window_bits;

    inflateEnd(&stream);
  }

  DestroyBlock(input);
}

// A wrong wrapper checksum has to be reported as a data error, and it is the
// no-input drain call that reports it.  ISA-L pulls bytes into its own state as
// soon as it needs bits, so a decode that hands its output back in pieces has
// consumed the whole compressed stream -- trailer included -- several calls
// before it finishes producing.  The call that finally reaches the checksum
// therefore arrives with avail_in == 0, and that path used to answer
// Z_BUF_ERROR whatever went wrong: an invitation to enlarge the buffer and
// retry a stream that is corrupt.  The zlib format is what reaches it, because
// its 4-byte trailer fits inside what ISA-L has already read ahead; gzip's
// 8-byte trailer does not, so that stream fails while input is still with the
// caller and takes the ordinary dispatch path instead.  (ISA-L does not latch
// the failure, so what a *repeat* call answers is a separate matter, decided
// where the sticky error state lives.)
TEST(IGZIPInflateRegressionTest, WrongChecksumIsADataErrorOnTheDrainCall) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 16 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, 0x3f04);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);
  ASSERT_GT(compressed.size(), 0u);

  // Only the last trailer byte, so every deflate block stays valid and the
  // decode runs to completion before anything is wrong.
  compressed[compressed.size() - 1] =
      static_cast<char>(compressed[compressed.size() - 1] ^ 0xff);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);
  stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  stream.avail_in = static_cast<uInt>(compressed.size());

  // Chunks small enough that the payload cannot be delivered in one call, which
  // is what puts the checksum check on a call with no input left.
  std::vector<char> chunk(1024);
  std::string decompressed;
  int ret = Z_OK;
  uInt avail_in_on_failing_call = 1;
  size_t produced_on_failing_call = 0;
  for (int call = 0; call < 4096; ++call) {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    avail_in_on_failing_call = stream.avail_in;
    ret = inflate(&stream, Z_SYNC_FLUSH);
    ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP) << "call=" << call;
    const size_t produced = chunk.size() - stream.avail_out;
    decompressed.append(chunk.data(), produced);
    if (ret != Z_OK) {
      produced_on_failing_call = produced;
      break;
    }
  }

  EXPECT_EQ(ret, Z_DATA_ERROR);
  EXPECT_EQ(avail_in_on_failing_call, 0u);

  // The failing call is the one that delivers the tail of the payload -- the
  // trailer is only checked once the last payload byte is out -- so it has to
  // account for those bytes as zlib does, whatever it goes on to return.
  // Reporting the error with next_out, avail_out and total_out untouched tells
  // the caller the bytes in its buffer do not exist.
  EXPECT_GT(produced_on_failing_call, 0u);
  EXPECT_EQ(stream.total_out, input_length);
  ASSERT_EQ(decompressed.size(), input_length);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_length), 0);

  inflateEnd(&stream);
  DestroyBlock(input);
}

// The same failure, called again. ISA-L holds no equivalent of zlib's BAD
// state, and on this path the repeat call reaches the drain site and reports
// the completion the checksum denied -- so without a latch the stream answers
// Z_STREAM_END, records its terminal state and answers Z_STREAM_END forever.
TEST(IGZIPInflateRegressionTest, DrainCallDataErrorStaysADataError) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 16 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, 0x3f04);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);
  ASSERT_GT(compressed.size(), 0u);
  compressed[compressed.size() - 1] =
      static_cast<char>(compressed[compressed.size() - 1] ^ 0xff);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);
  stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
  stream.avail_in = static_cast<uInt>(compressed.size());

  std::vector<char> chunk(1024);
  int ret = Z_OK;
  for (int call = 0; call < 4096; ++call) {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    ret = inflate(&stream, Z_SYNC_FLUSH);
    if (ret != Z_OK) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_DATA_ERROR);
  ASSERT_EQ(stream.avail_in, 0u);

  const uLong total_out_at_failure = stream.total_out;
  for (int repeat = 0; repeat < 2; ++repeat) {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    EXPECT_EQ(inflate(&stream, Z_SYNC_FLUSH), Z_DATA_ERROR)
        << "repeat=" << repeat;
    EXPECT_EQ(stream.total_out, total_out_at_failure) << "repeat=" << repeat;
  }

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

#ifdef USE_IAA
// IAA's decompressor has a fixed 4 kB history buffer, so it cannot decode a
// stream whose producer used a larger window -- zlib's default is 32 kB. QPL
// reports that as QPL_STS_BAD_DIST_ERR, and it is the one decompress failure
// that predicts the next call: the window belongs to the compressor, not to the
// block. IsIAADecompressible() cannot see it for raw deflate or gzip, where
// there is no header to read the window out of, so the only way to know is to
// be told once and remember.
class IAAWindowRejectionTest : public ::testing::Test {};

// Run one whole stream through strm and check the bytes. Returns the last
// inflate() code so the caller can assert on it, or Z_DATA_ERROR if the output
// came back wrong.
static int InflateWholeStream(z_streamp strm, const std::string& compressed,
                              const char* expected, size_t expected_length) {
  std::vector<Bytef> output(expected_length + 1024);
  strm->next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
  strm->avail_in = static_cast<uInt>(compressed.size());
  strm->next_out = output.data();
  strm->avail_out = static_cast<uInt>(output.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
      break;
    }
  }
  if (ret != Z_STREAM_END) {
    return ret;
  }
  if (strm->total_out != expected_length ||
      memcmp(output.data(), expected, expected_length) != 0) {
    return Z_DATA_ERROR;
  }
  return Z_STREAM_END;
}

// Every case below except the first needs QPL to get far enough into a job to
// report the oversized window. Without a usable device it fails at job
// initialization instead, which leaves the flag correctly clear -- so the test
// would be asserting the opposite of what it means to. Probe with a stream IAA
// can definitely decode: a short one, whose matches cannot reach back 4 kB
// because the whole payload is smaller than that.
static bool IAAHardwareDecompressWorks() {
  const size_t input_length = 2048;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x4144);
  if (input == nullptr) {
    return false;
  }
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);
  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  int ret = ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &compress_path);
  DestroyBlock(input);
  if (ret != Z_STREAM_END) {
    return false;
  }

  std::vector<uint8_t> output(input_length + 1024);
  uint32_t input_len = static_cast<uint32_t>(compressed.size());
  uint32_t output_len = static_cast<uint32_t>(output.size());
  bool end_of_stream = false;
  ret = UncompressIAA(reinterpret_cast<uint8_t*>(&compressed[0]), &input_len,
                      output.data(), &output_len, qpl_path_hardware,
                      /*window_bits=*/-15, &end_of_stream);
  return ret == 0 && end_of_stream && output_len == input_length;
}

// The contract UncompressIAA() now offers its callers, checked on QPL's
// software path so that it holds on a host with no device: the software path
// rejects an oversized window for the same reason and with the same status.
TEST_F(IAAWindowRejectionTest, UncompressIAAReportsOversizedWindow) {
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x7e11);
  ASSERT_NE(input, nullptr);

  // Two encodings of the same bytes. GenerateSeededCompressibleBlock() repeats
  // a string every 8192 bytes, so with zlib's full window the first is
  // guaranteed to contain a match distance IAA cannot reach; restricted to 4
  // kB, the second cannot contain one.
  std::string wide;
  std::string narrow;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &wide, -15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);
  ASSERT_EQ(ZlibCompress(input, input_length, &narrow, -12, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  std::vector<uint8_t> output(input_length + 1024);
  uint32_t input_len = static_cast<uint32_t>(wide.size());
  uint32_t output_len = static_cast<uint32_t>(output.size());
  bool end_of_stream = false;
  bool window_too_large = false;
  EXPECT_NE(UncompressIAA(reinterpret_cast<uint8_t*>(&wide[0]), &input_len,
                          output.data(), &output_len, qpl_path_software,
                          /*window_bits=*/-15, &end_of_stream,
                          /*detect_gzip_ext=*/false, &window_too_large),
            0);
  EXPECT_TRUE(window_too_large);

  // A stream IAA can follow decodes, and leaves the flag alone. Never setting
  // it to false is what lets a caller pass one bool through a whole stream.
  input_len = static_cast<uint32_t>(narrow.size());
  output_len = static_cast<uint32_t>(output.size());
  end_of_stream = false;
  bool narrow_window_too_large = false;
  EXPECT_EQ(UncompressIAA(reinterpret_cast<uint8_t*>(&narrow[0]), &input_len,
                          output.data(), &output_len, qpl_path_software,
                          /*window_bits=*/-12, &end_of_stream,
                          /*detect_gzip_ext=*/false, &narrow_window_too_large),
            0);
  EXPECT_FALSE(narrow_window_too_large);
  EXPECT_TRUE(end_of_stream);
  EXPECT_EQ(output_len, input_length);
  EXPECT_EQ(memcmp(output.data(), input, input_length), 0);

  // Omitting the out-parameter has to stay legal: most callers do not care
  // which failure they got.
  input_len = static_cast<uint32_t>(wide.size());
  output_len = static_cast<uint32_t>(output.size());
  end_of_stream = false;
  EXPECT_NE(UncompressIAA(reinterpret_cast<uint8_t*>(&wide[0]), &input_len,
                          output.data(), &output_len, qpl_path_software,
                          /*window_bits=*/-15, &end_of_stream),
            0);

  DestroyBlock(input);
}

// The point of the whole change. A rejection has to outlive inflateReset(),
// because a reset is exactly what the callers that matter do between documents:
// Lucene resets its Inflater once per stored field. Clearing the flag on reset
// would forget the lesson before it was ever acted on.
TEST_F(IAAWindowRejectionTest, StreamFlagSurvivesInflateReset) {
  if (!IAAHardwareDecompressWorks()) {
    GTEST_SKIP() << "no usable IAA device: QPL cannot reach the point where it "
                    "reports an oversized history window";
  }
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(IAA, /*zlib_fallback=*/true, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x7e12);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);
  EXPECT_FALSE(InflateIAAWindowRejected(&stream));

  // The rejection costs one submission and then falls through to zlib, so the
  // bytes are still right.
  EXPECT_EQ(InflateWholeStream(&stream, compressed, input, input_length),
            Z_STREAM_END);
  EXPECT_TRUE(InflateIAAWindowRejected(&stream));

  ASSERT_EQ(inflateReset(&stream), Z_OK);
  EXPECT_TRUE(InflateIAAWindowRejected(&stream));
  // inflateReset() clears the path, so a stream that had forgotten the
  // rejection would be dispatched to IAA again here.
  EXPECT_EQ(GetInflateExecutionPath(&stream), UNDEFINED);
  EXPECT_EQ(InflateWholeStream(&stream, compressed, input, input_length),
            Z_STREAM_END);
  EXPECT_NE(GetInflateExecutionPath(&stream), IAA);
  EXPECT_TRUE(InflateIAAWindowRejected(&stream));

  // inflateReset2() restarts the stream in a new format, and is the other way
  // back to an undefined path.
  ASSERT_EQ(inflateReset2(&stream, -15), Z_OK);
  EXPECT_TRUE(InflateIAAWindowRejected(&stream));

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

// A copy decodes the rest of the same stream, so it inherits the verdict. The
// settings are rebuilt member by member in SetFromCopy(), not assigned, so this
// is the kind of field that gets silently dropped.
TEST_F(IAAWindowRejectionTest, StreamFlagPropagatesThroughInflateCopy) {
  if (!IAAHardwareDecompressWorks()) {
    GTEST_SKIP() << "no usable IAA device: QPL cannot reach the point where it "
                    "reports an oversized history window";
  }
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(IAA, /*zlib_fallback=*/true, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x7e13);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, -15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream source;
  memset(&source, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&source, -15), Z_OK);
  EXPECT_EQ(InflateWholeStream(&source, compressed, input, input_length),
            Z_STREAM_END);
  ASSERT_TRUE(InflateIAAWindowRejected(&source));

  z_stream dest;
  memset(&dest, 0, sizeof(z_stream));
  ASSERT_EQ(inflateCopy(&dest, &source), Z_OK);
  EXPECT_TRUE(InflateIAAWindowRejected(&dest));
  // And the copy keeps it across its own reset, like the original.
  ASSERT_EQ(inflateReset(&dest), Z_OK);
  EXPECT_TRUE(InflateIAAWindowRejected(&dest));
  EXPECT_EQ(InflateWholeStream(&dest, compressed, input, input_length),
            Z_STREAM_END);
  EXPECT_NE(GetInflateExecutionPath(&dest), IAA);

  ASSERT_EQ(inflateEnd(&dest), Z_OK);
  ASSERT_EQ(inflateEnd(&source), Z_OK);
  DestroyBlock(input);
}

// A stream IAA can serve must not be tarred by another stream's rejection: the
// flag is per stream, and there is no process-wide counter behind it.
TEST_F(IAAWindowRejectionTest, RejectionDoesNotAffectOtherStreams) {
  if (!IAAHardwareDecompressWorks()) {
    GTEST_SKIP() << "no usable IAA device: QPL cannot reach the point where it "
                    "reports an oversized history window";
  }
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(IAA, /*zlib_fallback=*/true, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x7e14);
  ASSERT_NE(input, nullptr);

  std::string wide;
  std::string narrow;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &wide, -15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);
  ASSERT_EQ(ZlibCompress(input, input_length, &narrow, -12, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream rejected;
  memset(&rejected, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&rejected, -15), Z_OK);
  EXPECT_EQ(InflateWholeStream(&rejected, wide, input, input_length),
            Z_STREAM_END);
  ASSERT_TRUE(InflateIAAWindowRejected(&rejected));

  z_stream served;
  memset(&served, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&served, -15), Z_OK);
  EXPECT_EQ(InflateWholeStream(&served, narrow, input, input_length),
            Z_STREAM_END);
  EXPECT_FALSE(InflateIAAWindowRejected(&served));
  EXPECT_EQ(GetInflateExecutionPath(&served), IAA);

  ASSERT_EQ(inflateEnd(&served), Z_OK);
  ASSERT_EQ(inflateEnd(&rejected), Z_OK);
  DestroyBlock(input);
}

// The window a caller declares, independent of any hardware: the format's
// wrapper offset has to come off before the window is compared, or a gzip
// stream looks like a 24-bit window and a raw one like a negative window.
TEST_F(IAAWindowRejectionTest, DeclaresIAACompatibleWindowFollowsTheFormat) {
  // Raw deflate.
  EXPECT_TRUE(DeclaresIAACompatibleWindow(-8));
  EXPECT_TRUE(DeclaresIAACompatibleWindow(-12));
  EXPECT_FALSE(DeclaresIAACompatibleWindow(-13));
  EXPECT_FALSE(DeclaresIAACompatibleWindow(-15));
  // Zlib.
  EXPECT_TRUE(DeclaresIAACompatibleWindow(8));
  EXPECT_TRUE(DeclaresIAACompatibleWindow(12));
  EXPECT_FALSE(DeclaresIAACompatibleWindow(13));
  EXPECT_FALSE(DeclaresIAACompatibleWindow(15));
  // Gzip, which zlib selects by adding 16.
  EXPECT_TRUE(DeclaresIAACompatibleWindow(16 + 8));
  EXPECT_TRUE(DeclaresIAACompatibleWindow(16 + 12));
  EXPECT_FALSE(DeclaresIAACompatibleWindow(16 + 13));
  EXPECT_FALSE(DeclaresIAACompatibleWindow(16 + 15));
  // Anything this shim does not map to a format cannot be declared compatible,
  // including zlib's automatic-detection range, where the stream picks the
  // wrapper and the caller has therefore declared nothing.
  EXPECT_FALSE(DeclaresIAACompatibleWindow(0));
  EXPECT_FALSE(DeclaresIAACompatibleWindow(32 + 15));
  EXPECT_FALSE(DeclaresIAACompatibleWindow(-16));
}

// A narrowing inflateReset2() is the caller declaring the next stream's window,
// so it retires the verdict: the flag is an inference about the previous
// stream's producer, and a declaration outranks an inference. Without this a
// stream that was rejected once never reaches IAA again even after the caller
// has said the data cannot reference beyond 4 kB.
TEST_F(IAAWindowRejectionTest, NarrowingInflateReset2ClearsTheVerdict) {
  if (!IAAHardwareDecompressWorks()) {
    GTEST_SKIP() << "no usable IAA device: QPL cannot reach the point where it "
                    "reports an oversized history window";
  }
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(IAA, /*zlib_fallback=*/true, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x7e15);
  ASSERT_NE(input, nullptr);

  std::string wide;
  std::string narrow;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &wide, -15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);
  ASSERT_EQ(ZlibCompress(input, input_length, &narrow, -12, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);
  EXPECT_EQ(InflateWholeStream(&stream, wide, input, input_length),
            Z_STREAM_END);
  ASSERT_TRUE(InflateIAAWindowRejected(&stream));

  // Same window, so nothing has been declared and the verdict stands. This is
  // the control: without it the test would pass on a build that cleared the
  // flag on every reset.
  ASSERT_EQ(inflateReset2(&stream, -15), Z_OK);
  EXPECT_TRUE(InflateIAAWindowRejected(&stream));
  EXPECT_EQ(InflateWholeStream(&stream, narrow, input, input_length),
            Z_STREAM_END);
  EXPECT_NE(GetInflateExecutionPath(&stream), IAA);

  // Narrowing to a window IAA can follow retires it, and the next stream is
  // offered to IAA again -- and served, since the bytes really do stay inside
  // 4 kB.
  ASSERT_EQ(inflateReset2(&stream, -12), Z_OK);
  EXPECT_FALSE(InflateIAAWindowRejected(&stream));
  EXPECT_EQ(InflateWholeStream(&stream, narrow, input, input_length),
            Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), IAA);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#endif  // USE_IAA
