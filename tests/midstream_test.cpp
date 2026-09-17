// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// Suites covering state carried across calls on one stream: stream copies,
// terminal state, an unregistered stream and a mid-stream decode failure.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../config/config.h"
#include "../logging.h"
#include "../zlib_accel.h"
#include "test_utils.h"

using namespace config;

#ifdef USE_IGZIP
#include "../igzip.h"
#endif

#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)
class StreamCopyRegressionTest : public ::testing::Test {};

// deflateCopy()/inflateCopy() duplicate zlib's stream state, but the shim keeps
// its own per-stream state in maps keyed by z_streamp, so before these were
// intercepted the copy had no entry at all.  Since the null guards landed, an
// untracked z_streamp no longer crashes -- it silently runs on orig_deflate /
// orig_inflate instead, which loses the offload outright and, with the source
// midstream on IGZIP, produces output that does not inflate.
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

// The one case that cannot be copied at all -- see deflateCopy() for why
// ISA-L's state cannot be duplicated.  Because the call is refused before
// orig_deflateCopy runs, dest is left exactly as the caller passed it and the
// source is untouched.
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
// released explicitly or it leaks.  The leak is only visible to a memory
// checker -- the replacement entry reads as owning nothing either way -- so
// what these two cases pin is the other half: the copy owns no ISA-L state of
// its own, which is what keeps the two streams from sharing one, and releasing
// the old state does not disturb the zlib state the copy has to continue from.
// Reaching the state at all depends on deflateReset()/inflateReset() keeping
// the ISA-L stream.
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

  // zlib's deflateCopy overwrites the destination z_stream wholesale, so the
  // destination's own zlib state is orphaned rather than freed -- stock zlib
  // does this with no shim loaded.  Keep the pointer so the test can hand it
  // back afterwards; deflateEnd refuses any other z_stream address, because the
  // state points back at the stream it was initialized with.
  struct internal_state* orphaned_state = dest.state;

  ASSERT_EQ(deflateCopy(&dest, &source), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&dest), ZLIB);
  EXPECT_FALSE(DeflateOwnsIgzipState(&dest));
  ASSERT_NE(dest.state, orphaned_state);

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
  dest.state = orphaned_state;
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

  // As on the deflate side, the copy orphans the destination's own zlib state,
  // which only this z_stream address can release.
  struct internal_state* orphaned_state = dest.state;

  ASSERT_EQ(inflateCopy(&dest, &source), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&dest), ZLIB);
  EXPECT_FALSE(InflateOwnsIgzipState(&dest));
  ASSERT_NE(dest.state, orphaned_state);

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
  dest.state = orphaned_state;
  ASSERT_EQ(inflateEnd(&dest), Z_OK);
  ASSERT_EQ(inflateEnd(&source), Z_OK);

  DestroyBlock(input);
}
#endif  // USE_IGZIP
#endif  // USE_IGZIP || USE_QAT || USE_IAA

#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)
class TerminalStateRegressionTest : public ::testing::Test {};

// An offloaded stream never feeds zlib's own deflate/inflate state, so before
// the shim recorded that a stream had ended, every call after Z_STREAM_END was
// dispatched from scratch: QAT and IAA appended bytes zlib would never emit --
// a complete empty stream after a finished one, or a second header -- while
// IGZIP consumed the whole input, wrote nothing and reported success, which is
// data loss behind a success code.
//
// The expectations below are zlib's own, measured against a binary that does
// not link the shim rather than read out of deflate.c, because zlib validates
// its parameters *before* it reports the terminal state and the order matters:
// avail_out == 0 outranks Z_STREAM_END on the deflate side but not on the
// inflate side, a null next_out outranks it on both, a null next_in with input
// outranks the "more input after FINISH" Z_BUF_ERROR, and inflate accepts any
// flush value where deflate rejects everything but Z_FINISH.
// strm->msg is part of the same measurement. zlib only writes it where it
// rejects through ERR_RETURN, so deflate() leaves it alone on the one row it
// answers from its first parameter check -- the flush range -- as well as on
// the row it accepts, and inflate() leaves it alone on every row, both of its
// pointer rejections being plain returns. expected_deflate_msg is null for "not
// written"; inflate needs no column, having no row that writes it.
struct PostEndRow {
  const char* label;
  int flush;
  bool avail_in_nonzero;
  bool avail_out_zero;
  bool null_next_out;
  bool null_next_in;
  int expected_deflate;
  int expected_inflate;
  const char* expected_deflate_msg;
};

static const PostEndRow kPostEndRows[] = {
    {"avail_in>0, Z_FINISH", Z_FINISH, true, false, false, false, Z_BUF_ERROR,
     Z_STREAM_END, "buffer error"},
    {"avail_in>0, Z_NO_FLUSH", Z_NO_FLUSH, true, false, false, false,
     Z_STREAM_ERROR, Z_STREAM_END, "stream error"},
    {"avail_in=0, Z_FINISH", Z_FINISH, false, false, false, false, Z_STREAM_END,
     Z_STREAM_END, nullptr},
    {"avail_in=0, Z_NO_FLUSH", Z_NO_FLUSH, false, false, false, false,
     Z_STREAM_ERROR, Z_STREAM_END, "stream error"},
    {"avail_in=0, Z_SYNC_FLUSH", Z_SYNC_FLUSH, false, false, false, false,
     Z_STREAM_ERROR, Z_STREAM_END, "stream error"},
    {"avail_in=0, Z_BLOCK", Z_BLOCK, false, false, false, false, Z_STREAM_ERROR,
     Z_STREAM_END, "stream error"},
    {"avail_in=0, flush out of range", 99, false, false, false, false,
     Z_STREAM_ERROR, Z_STREAM_END, nullptr},
    {"avail_in=0, Z_FINISH, avail_out=0", Z_FINISH, false, true, false, false,
     Z_BUF_ERROR, Z_STREAM_END, "buffer error"},
    {"avail_in>0, Z_FINISH, avail_out=0", Z_FINISH, true, true, false, false,
     Z_BUF_ERROR, Z_STREAM_END, "buffer error"},
    {"avail_in=0, Z_FINISH, next_out=NULL", Z_FINISH, false, false, true, false,
     Z_STREAM_ERROR, Z_STREAM_ERROR, "stream error"},
    {"avail_in>0, Z_FINISH, next_in=NULL", Z_FINISH, true, false, false, true,
     Z_STREAM_ERROR, Z_STREAM_ERROR, "stream error"},
};

static const size_t kPostEndRowCount =
    sizeof(kPostEndRows) / sizeof(kPostEndRows[0]);

// Written into strm->msg before each row so "zlib left it alone" is
// distinguishable from "zlib set it to null". A string literal, so nothing ever
// frees it -- zlib only ever points msg at z_errmsg[] entries, never owns it.
static const char* const kMsgSentinel = "zlib-accel test sentinel";

// Every row has to leave the stream exactly as it was, so total_in and
// total_out are checked alongside the return code: a return code that happens
// to match while bytes were still written is the failure mode this is guarding.
static void CheckDeflatePostEndRows(z_streamp stream, char* input,
                                    size_t input_length, Bytef* spare,
                                    size_t spare_length) {
  const uLong total_in_before = stream->total_in;
  const uLong total_out_before = stream->total_out;
  // deflate() never writes data_type, on any path, so no row may either.
  const int data_type_before = stream->data_type;

  for (size_t i = 0; i < kPostEndRowCount; i++) {
    const PostEndRow& row = kPostEndRows[i];
    stream->next_in =
        row.null_next_in ? nullptr : reinterpret_cast<Bytef*>(input);
    stream->avail_in =
        row.avail_in_nonzero ? static_cast<uInt>(input_length) : 0;
    stream->next_out = row.null_next_out ? nullptr : spare;
    stream->avail_out =
        row.avail_out_zero ? 0 : static_cast<uInt>(spare_length);
    stream->msg = const_cast<char*>(kMsgSentinel);

    EXPECT_EQ(deflate(stream, row.flush), row.expected_deflate) << row.label;
    EXPECT_EQ(stream->total_in, total_in_before) << row.label;
    EXPECT_EQ(stream->total_out, total_out_before) << row.label;
    EXPECT_STREQ(stream->msg, row.expected_deflate_msg == nullptr
                                  ? kMsgSentinel
                                  : row.expected_deflate_msg)
        << row.label;
    EXPECT_EQ(stream->data_type, data_type_before) << row.label;
  }

  // Leave the stream holding valid pointers for whatever the caller does next.
  stream->next_in = reinterpret_cast<Bytef*>(input);
  stream->avail_in = 0;
  stream->next_out = spare;
  stream->avail_out = static_cast<uInt>(spare_length);
  stream->msg = nullptr;
}

static void CheckInflatePostEndRows(z_streamp stream,
                                    const std::string& compressed, Bytef* spare,
                                    size_t spare_length) {
  const uLong total_in_before = stream->total_in;
  const uLong total_out_before = stream->total_out;
  // One expectation that holds on every path, and the reason it does differs by
  // path: zlib recomputes data_type on each return, but from a stream it has
  // already finished it recomputes the same value it left there (or, on the two
  // rows it rejects, does not reach the recomputation at all), while an
  // offloaded stream never had the field written and the gate does not start.
  // Either way the rows must not move it -- and this catches a gate that begins
  // guessing a value, which would be indistinguishable from a real one to a
  // caller. See the README on why data_type is left alone when offloading.
  const int data_type_before = stream->data_type;
  Bytef* compressed_bytes =
      reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));

  for (size_t i = 0; i < kPostEndRowCount; i++) {
    const PostEndRow& row = kPostEndRows[i];
    stream->next_in = row.null_next_in ? nullptr : compressed_bytes;
    stream->avail_in =
        row.avail_in_nonzero ? static_cast<uInt>(compressed.size()) : 0;
    stream->next_out = row.null_next_out ? nullptr : spare;
    stream->avail_out =
        row.avail_out_zero ? 0 : static_cast<uInt>(spare_length);
    stream->msg = const_cast<char*>(kMsgSentinel);

    EXPECT_EQ(inflate(stream, row.flush), row.expected_inflate) << row.label;
    EXPECT_EQ(stream->total_in, total_in_before) << row.label;
    EXPECT_EQ(stream->total_out, total_out_before) << row.label;
    EXPECT_STREQ(stream->msg, kMsgSentinel) << row.label;
    EXPECT_EQ(stream->data_type, data_type_before) << row.label;
  }

  stream->next_in = compressed_bytes;
  stream->avail_in = 0;
  stream->next_out = spare;
  stream->avail_out = static_cast<uInt>(spare_length);
  stream->msg = nullptr;
}

static void RunDeflatePostEndMatchesZlib(ExecutionPath accel_path) {
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

  const size_t bound = deflateBound(&stream, input_length) + 4096;
  std::vector<Bytef> output(bound);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const size_t produced = output.size() - stream.avail_out;
  ASSERT_EQ(GetDeflateExecutionPath(&stream), accel_path);
  const std::vector<Bytef> first(output.begin(), output.begin() + produced);

  // The spare buffer is deliberately separate from the finished stream's
  // output: anything written into it would have landed after a complete stream.
  std::vector<Bytef> spare(4096);
  CheckDeflatePostEndRows(&stream, input, input_length, spare.data(),
                          spare.size());

  // The finished stream itself has to be untouched and still decodable.
  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  size_t input_consumed = 0;
  ExecutionPath uncompress_path = UNDEFINED;
  ASSERT_EQ(
      ZlibUncompress(reinterpret_cast<const char*>(first.data()), first.size(),
                     input_length, &uncompressed, &uncompressed_length,
                     &input_consumed, 15, Z_FINISH, 1, &uncompress_path),
      Z_STREAM_END);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);
  delete[] uncompressed;

  // deflateReset clears the terminal state, so the same stream compresses
  // again -- and byte-identically, since a reset stream is back where it
  // started. A flag left set here would wedge the stream at Z_STREAM_END.
  ASSERT_EQ(deflateReset(&stream), Z_OK);
  std::vector<Bytef> again(bound);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = again.data();
  stream.avail_out = static_cast<uInt>(again.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  again.resize(stream.total_out);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), accel_path);
  EXPECT_EQ(again, first);

  ASSERT_EQ(deflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPDeflatePostEndMatchesZlib) {
  RunDeflatePostEndMatchesZlib(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATDeflatePostEndMatchesZlib) {
  RunDeflatePostEndMatchesZlib(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAADeflatePostEndMatchesZlib) {
  RunDeflatePostEndMatchesZlib(IAA);
}
#endif

static void RunInflatePostEndMatchesZlib(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  // Compressed by the same backend so the stream is one it will decompress.
  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = uncompressed.data();
  stream.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&stream), accel_path);
  ASSERT_EQ(stream.total_out, input_length);

  // Re-feeding the whole stream decoded it a second time on QAT and IAA, which
  // is what these rows rule out.
  std::vector<Bytef> spare(4096);
  CheckInflatePostEndRows(&stream, compressed, spare.data(), spare.size());

  // inflateReset clears the terminal state, so the stream decodes again.
  ASSERT_EQ(inflateReset(&stream), Z_OK);
  std::vector<Bytef> again(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = again.data();
  stream.avail_out = static_cast<uInt>(again.size());
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(again.data(), input, input_length), 0);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPInflatePostEndMatchesZlib) {
  RunInflatePostEndMatchesZlib(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATInflatePostEndMatchesZlib) {
  RunInflatePostEndMatchesZlib(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAAInflatePostEndMatchesZlib) {
  RunInflatePostEndMatchesZlib(IAA);
}
#endif

// The review comment on the stream-copy work that started this: a copy of a
// finished stream held a zlib deflate state the accelerator had never fed, so
// feeding the copy produced a second stream where zlib returns Z_BUF_ERROR and
// writes nothing. The copy inherits the terminal state, so it now refuses input
// exactly as its source does.
static void RunFinishedDeflateCopyRefusesInput(ExecutionPath accel_path) {
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

  const size_t bound = deflateBound(&source, input_length) + 4096;
  std::vector<Bytef> output(bound);
  source.next_in = reinterpret_cast<Bytef*>(input);
  source.avail_in = static_cast<uInt>(input_length);
  source.next_out = output.data();
  source.avail_out = static_cast<uInt>(output.size());
  ASSERT_EQ(deflate(&source, Z_FINISH), Z_STREAM_END);
  ASSERT_EQ(GetDeflateExecutionPath(&source), accel_path);

  z_stream copy;
  memset(&copy, 0, sizeof(z_stream));
  ASSERT_EQ(deflateCopy(&copy, &source), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&copy), accel_path);

  std::vector<Bytef> spare(4096);
  CheckDeflatePostEndRows(&copy, input, input_length, spare.data(),
                          spare.size());

  ASSERT_EQ(deflateEnd(&copy), Z_OK);
  ASSERT_EQ(deflateEnd(&source), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATFinishedDeflateCopyRefusesInput) {
  RunFinishedDeflateCopyRefusesInput(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAAFinishedDeflateCopyRefusesInput) {
  RunFinishedDeflateCopyRefusesInput(IAA);
}
#endif

#ifdef USE_IGZIP
// A finished IGZIP stream is copyable for the same reason the copy needs no
// ISA-L state: ISA-L is at ZSTATE_END with all output delivered, and the
// terminal state the copy inherits answers every call on it. Only a live IGZIP
// stream is refused, which IGZIPRefusesMidstreamDeflateCopy covers.
TEST_F(TerminalStateRegressionTest, IGZIPFinishedDeflateCopyRefusesInput) {
  RunFinishedDeflateCopyRefusesInput(IGZIP);
}
#endif  // USE_IGZIP

static void RunFinishedInflateCopyReturnsStreamEnd(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
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

  std::vector<Bytef> uncompressed(input_length + 1024);
  source.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  source.avail_in = static_cast<uInt>(compressed.size());
  source.next_out = uncompressed.data();
  source.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&source, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&source), accel_path);

  z_stream copy;
  memset(&copy, 0, sizeof(z_stream));
  ASSERT_EQ(inflateCopy(&copy, &source), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&copy), accel_path);

  std::vector<Bytef> spare(4096);
  CheckInflatePostEndRows(&copy, compressed, spare.data(), spare.size());

  ASSERT_EQ(inflateEnd(&copy), Z_OK);
  ASSERT_EQ(inflateEnd(&source), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPFinishedInflateCopyReturnsStreamEnd) {
  RunFinishedInflateCopyReturnsStreamEnd(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATFinishedInflateCopyReturnsStreamEnd) {
  RunFinishedInflateCopyReturnsStreamEnd(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAAFinishedInflateCopyReturnsStreamEnd) {
  RunFinishedInflateCopyReturnsStreamEnd(IAA);
}
#endif

// inflateReset2 is the one API that restarts a finished stream without going
// through inflateReset, so a terminal-state flag it did not clear would wedge
// the stream at Z_STREAM_END forever -- a failure this fix would introduce if
// inflateReset2 were left unintercepted.
static void RunInflateReset2ClearsStreamEnd(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = uncompressed.data();
  stream.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&stream), accel_path);

  // Same windowBits, so nothing changes but the path and the terminal state.
  ASSERT_EQ(inflateReset2(&stream, 15), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), UNDEFINED);

  std::vector<Bytef> again(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = again.data();
  stream.avail_out = static_cast<uInt>(again.size());
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), accel_path);
  EXPECT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(again.data(), input, input_length), 0);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPInflateReset2ClearsStreamEnd) {
  RunInflateReset2ClearsStreamEnd(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATInflateReset2ClearsStreamEnd) {
  RunInflateReset2ClearsStreamEnd(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAAInflateReset2ClearsStreamEnd) {
  RunInflateReset2ClearsStreamEnd(IAA);
}
#endif

// deflateResetKeep is the third entry point that restarts a finished stream
// without going through deflateReset. What it keeps is zlib's LZ77 window,
// which says nothing about whether the next stream may be offloaded, so it
// clears the terminal state and nothing else.
static void RunDeflateResetKeepClearsStreamEnd(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11d1);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  const size_t bound = deflateBound(&stream, input_length) + 4096;
  std::vector<Bytef> output(bound);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  ASSERT_EQ(GetDeflateExecutionPath(&stream), accel_path);
  output.resize(stream.total_out);

  ASSERT_EQ(deflateResetKeep(&stream), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), UNDEFINED);

  std::vector<Bytef> again(bound);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = again.data();
  stream.avail_out = static_cast<uInt>(again.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  again.resize(stream.total_out);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), accel_path);
  // The offload emits no back-references into the previous stream, so the kept
  // window makes no difference to what it produces.
  EXPECT_EQ(again, output);

  ASSERT_EQ(deflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPDeflateResetKeepClearsStreamEnd) {
  RunDeflateResetKeepClearsStreamEnd(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATDeflateResetKeepClearsStreamEnd) {
  RunDeflateResetKeepClearsStreamEnd(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAADeflateResetKeepClearsStreamEnd) {
  RunDeflateResetKeepClearsStreamEnd(IAA);
}
#endif

// inflateResetKeep keeps the window the previous stream built, so the next
// stream may reference bytes no backend can see. It clears the terminal state
// like the other reset entry points and additionally pins the stream to zlib.
// use_zlib_uncompress is off here, so the second stream only decodes if the pin
// itself reaches orig_inflate. The following inflateReset has to lift the pin
// -- it is the reset that discards the history -- which is also what would fail
// if the pin ever leaked out of a nested call into plain inflateReset.
static void RunInflateResetKeepClearsStreamEnd(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11d2);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = uncompressed.data();
  stream.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&stream), accel_path);

  ASSERT_EQ(inflateResetKeep(&stream), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);
  // The reset itself releases nothing: what makes ISA-L state unreachable is
  // the pin, and whether the pin holds is not settled until the next inflate()
  // -- inflateReset() may lift it first. Trivially false on QAT and IAA, which
  // never own an ISA-L stream.
  EXPECT_EQ(InflateOwnsIgzipState(&stream), accel_path == IGZIP);

  std::vector<Bytef> again(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = again.data();
  stream.avail_out = static_cast<uInt>(again.size());
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);
  EXPECT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(again.data(), input, input_length), 0);
  // Decoding under the pin is what settles it: the stream is on zlib for good
  // until a reset, so the ISA-L state went back rather than staying dormant
  // until inflateEnd().
  EXPECT_FALSE(InflateOwnsIgzipState(&stream));

  // A plain reset discards the window, so the stream is offloadable again.
  ASSERT_EQ(inflateReset(&stream), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), UNDEFINED);

  std::vector<Bytef> third(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = third.data();
  stream.avail_out = static_cast<uInt>(third.size());
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), accel_path);
  EXPECT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(third.data(), input, input_length), 0);
  // The release cost nothing: inflate() built a new state from a null isal_strm
  // as soon as inflateReset() lifted the pin.
  EXPECT_EQ(InflateOwnsIgzipState(&stream), accel_path == IGZIP);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPInflateResetKeepClearsStreamEnd) {
  RunInflateResetKeepClearsStreamEnd(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATInflateResetKeepClearsStreamEnd) {
  RunInflateResetKeepClearsStreamEnd(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAAInflateResetKeepClearsStreamEnd) {
  RunInflateResetKeepClearsStreamEnd(IAA);
}
#endif

#ifdef USE_IGZIP
// inflateResetKeep() followed straight by inflateReset(), with no inflate()
// between, is the sequence a libz whose internal calls are interposable
// produces for a plain inflateReset(): zlib's inflateReset() is
// inflateResetKeep() plus a discarded window, so the shim's own
// inflateResetKeep() runs first and pins, then the outer wrapper lifts the pin.
// An application can also just call the two in that order.
//
// The ISA-L state has to survive it. Releasing at the pin instead would free
// the state on every ordinary reset on such a libz -- the outer wrapper can
// restore the path it overwrote, but not an allocation. IGZIP only: QAT and IAA
// never own an ISA-L stream, so there would be nothing to observe.
TEST_F(TerminalStateRegressionTest, IGZIPInflateResetKeepThenResetKeepsState) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11d3);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = uncompressed.data();
  stream.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);
  ASSERT_TRUE(InflateOwnsIgzipState(&stream));

  ASSERT_EQ(inflateResetKeep(&stream), Z_OK);
  ASSERT_EQ(inflateReset(&stream), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), UNDEFINED);
  EXPECT_TRUE(InflateOwnsIgzipState(&stream));

  // And the state that survived is usable: the next stream decodes on IGZIP
  // rather than being rebuilt or refused.
  std::vector<Bytef> again(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = again.data();
  stream.avail_out = static_cast<uInt>(again.size());
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP);
  EXPECT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(again.data(), input, input_length), 0);
  EXPECT_TRUE(InflateOwnsIgzipState(&stream));

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}
#endif

// Drives inflate() over one entire stream and returns the code it settled on.
static int InflateWholeStream(z_stream* stream, std::vector<Bytef>* in,
                              Bytef* out, size_t out_length) {
  stream->next_in = in->data();
  stream->avail_in = static_cast<uInt>(in->size());
  stream->next_out = out;
  stream->avail_out = static_cast<uInt>(out_length);
  int ret = Z_OK;
  for (int guard = 0; guard < 128 && ret == Z_OK; guard++) {
    ret = inflate(stream, Z_NO_FLUSH);
  }
  return ret;
}

// What the pin after inflateResetKeep decides is which engine decodes the next
// stream, not what history that engine has: an offloaded stream never fed
// zlib's window, so a next stream that references its bytes cannot be decoded
// at all. The all-zlib decode is the oracle for that claim -- it decodes the
// same pair correctly -- so what the accelerated half pins is the failure mode:
// a zlib data error on the stream that needs the history, rather than a backend
// resolving the lookback against an unrelated window and reporting success.
static void RunInflateResetKeepHistoryDependentStreamFails(
    ExecutionPath accel_path) {
  SetCompressPath(ZLIB, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, /*zlib_fallback=*/false, false);
  // The whole construction is a preset dictionary, so it needs the option that
  // makes deflateSetDictionary a no-op turned off. It comes from the config
  // file rather than from the path helpers, so a host that sets it would
  // otherwise get a self-contained second stream and no history dependence at
  // all.
  const uint32_t saved_ignore_dictionary = GetConfig(IGNORE_ZLIB_DICTIONARY);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  const size_t input_length = 64 * 1024;
  const size_t tail_length = 8 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11d3);
  ASSERT_NE(input, nullptr);

  // A stream that only decodes against another stream's output. Raw deflate,
  // because a preset dictionary sets the encoder's window there without an
  // FDICT bit: the second stream is then ordinary deflate whose distances reach
  // behind its own first byte, which is exactly what a window retained across
  // inflateResetKeep is supposed to supply.
  z_stream first_stream;
  memset(&first_stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&first_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15,
                         8, Z_DEFAULT_STRATEGY),
            Z_OK);
  std::vector<Bytef> first(deflateBound(&first_stream, input_length) + 128);
  first_stream.next_in = reinterpret_cast<Bytef*>(input);
  first_stream.avail_in = static_cast<uInt>(input_length);
  first_stream.next_out = first.data();
  first_stream.avail_out = static_cast<uInt>(first.size());
  ASSERT_EQ(deflate(&first_stream, Z_FINISH), Z_STREAM_END);
  first.resize(first_stream.total_out);
  ASSERT_EQ(deflateEnd(&first_stream), Z_OK);

  z_stream second_stream;
  memset(&second_stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&second_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15,
                         8, Z_DEFAULT_STRATEGY),
            Z_OK);
  ASSERT_EQ(deflateSetDictionary(&second_stream,
                                 reinterpret_cast<const Bytef*>(input),
                                 static_cast<uInt>(input_length)),
            Z_OK);
  std::vector<Bytef> second(deflateBound(&second_stream, tail_length) + 128);
  second_stream.next_in =
      reinterpret_cast<Bytef*>(input + input_length - tail_length);
  second_stream.avail_in = static_cast<uInt>(tail_length);
  second_stream.next_out = second.data();
  second_stream.avail_out = static_cast<uInt>(second.size());
  ASSERT_EQ(deflate(&second_stream, Z_FINISH), Z_STREAM_END);
  second.resize(second_stream.total_out);
  ASSERT_EQ(deflateEnd(&second_stream), Z_OK);
  // Nothing below sets a dictionary, and this is ahead of every early exit.
  SetConfig(IGNORE_ZLIB_DICTIONARY, saved_ignore_dictionary);

  std::vector<Bytef> out(input_length + 1024);

  // The dependence itself, without which the rest of the test proves nothing:
  // on its own the second stream is not decodable at all.
  z_stream standalone;
  memset(&standalone, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&standalone, -15), Z_OK);
  ASSERT_EQ(InflateWholeStream(&standalone, &second, out.data(), out.size()),
            Z_DATA_ERROR);
  ASSERT_EQ(inflateEnd(&standalone), Z_OK);

  // Oracle: decoded end to end by zlib, which does hold the history, the pair
  // round-trips.
  z_stream oracle;
  memset(&oracle, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&oracle, -15), Z_OK);
  ASSERT_EQ(InflateWholeStream(&oracle, &first, out.data(), out.size()),
            Z_STREAM_END);
  ASSERT_EQ(oracle.total_out, input_length);
  ASSERT_EQ(inflateResetKeep(&oracle), Z_OK);
  ASSERT_EQ(InflateWholeStream(&oracle, &second, out.data(), out.size()),
            Z_STREAM_END);
  EXPECT_EQ(oracle.total_out, tail_length);
  EXPECT_EQ(memcmp(out.data(), input + input_length - tail_length, tail_length),
            0);
  ASSERT_EQ(inflateEnd(&oracle), Z_OK);

  // Offloaded, the same second stream has no history to decode against.
  SetUncompressPath(accel_path, /*zlib_fallback=*/true, false);
  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, -15), Z_OK);
  ASSERT_EQ(InflateWholeStream(&stream, &first, out.data(), out.size()),
            Z_STREAM_END);
  ASSERT_EQ(stream.total_out, input_length);
  if (GetInflateExecutionPath(&stream) != accel_path) {
    // Not every backend can decode what zlib produced with a 32 KiB window, and
    // a stream that fell back to zlib fed zlib's window -- the case the oracle
    // above already covers.
    ASSERT_EQ(inflateEnd(&stream), Z_OK);
    DestroyBlock(input);
    GTEST_SKIP() << "first stream was not offloaded to the path under test";
  }

  ASSERT_EQ(inflateResetKeep(&stream), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);
  EXPECT_EQ(InflateWholeStream(&stream, &second, out.data(), out.size()),
            Z_DATA_ERROR);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest,
       IGZIPInflateResetKeepHistoryDependentStreamFails) {
  RunInflateResetKeepHistoryDependentStreamFails(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest,
       QATInflateResetKeepHistoryDependentStreamFails) {
  RunInflateResetKeepHistoryDependentStreamFails(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest,
       IAAInflateResetKeepHistoryDependentStreamFails) {
  RunInflateResetKeepHistoryDependentStreamFails(IAA);
}
#endif

#ifdef USE_IGZIP
// The IGZIP inflate path reports completion in two places. With avail_in == 0
// and output still buffered inside ISA-L, inflate() drains that state and
// returns Z_STREAM_END there rather than from the translation block, so a
// stream that ends this way has to end up in the same terminal state. A small
// first avail_out is what leaves ISA-L holding the output: it decompresses into
// its own tmp_out_buffer, consumes the whole input doing so, and then has more
// to give than the caller had room for.
TEST_F(TerminalStateRegressionTest, IGZIPInflatePostEndAfterDrainMatchesZlib) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = uncompressed.data();
  stream.avail_out = 1024;
  ASSERT_EQ(inflate(&stream, Z_NO_FLUSH), Z_OK);
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);
  ASSERT_EQ(stream.avail_in, 0u);

  // Every call from here on has avail_in == 0, so the completion can only come
  // from the drain site.
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    stream.next_out = uncompressed.data() + stream.total_out;
    stream.avail_out =
        static_cast<uInt>(uncompressed.size() - stream.total_out);
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);

  std::vector<Bytef> spare(4096);
  CheckInflatePostEndRows(&stream, compressed, spare.data(), spare.size());

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}
#endif  // USE_IGZIP

// The other half of intercepting inflateReset2: it is the only entry point that
// changes windowBits on a live stream, so the recorded window_bits has to be
// refreshed. Left stale, path selection keeps deciding on the old format and
// hands raw deflate data to a backend expecting a zlib header.
static void RunInflateReset2RefreshesWindowBits(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string zlib_stream;
  std::string raw_stream;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &zlib_stream, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);
  ASSERT_EQ(ZlibCompress(input, input_length, &raw_stream, -15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&zlib_stream[0]);
  stream.avail_in = static_cast<uInt>(zlib_stream.size());
  stream.next_out = uncompressed.data();
  stream.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&stream), accel_path);

  // Raw deflate from here on. With use_zlib_uncompress=0 there is no fallback
  // to hide a stale format behind.
  ASSERT_EQ(inflateReset2(&stream, -15), Z_OK);

  std::vector<Bytef> again(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&raw_stream[0]);
  stream.avail_in = static_cast<uInt>(raw_stream.size());
  stream.next_out = again.data();
  stream.avail_out = static_cast<uInt>(again.size());
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(again.data(), input, input_length), 0);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPInflateReset2RefreshesWindowBits) {
  RunInflateReset2RefreshesWindowBits(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATInflateReset2RefreshesWindowBits) {
  RunInflateReset2RefreshesWindowBits(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAAInflateReset2RefreshesWindowBits) {
  RunInflateReset2RefreshesWindowBits(IAA);
}
#endif

#ifdef USE_IGZIP
// isal_inflate_reset() deliberately preserves crc_flag and hist_bits, so an
// ISA-L stream cannot be reset across a format change -- it has to be
// discarded. Kept and merely reset, it would decode the gzip stream below with
// crc_flag still IGZIP_ZLIB.
TEST_F(TerminalStateRegressionTest,
       IGZIPInflateReset2RebuildsStreamOnFormatChange) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  std::string zlib_stream;
  std::string gzip_stream;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, input_length, &zlib_stream, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);
  ASSERT_EQ(ZlibCompress(input, input_length, &gzip_stream, 31, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&zlib_stream[0]);
  stream.avail_in = static_cast<uInt>(zlib_stream.size());
  stream.next_out = uncompressed.data();
  stream.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);

  ASSERT_EQ(inflateReset2(&stream, 31), Z_OK);

  std::vector<Bytef> again(input_length + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&gzip_stream[0]);
  stream.avail_in = static_cast<uInt>(gzip_stream.size());
  stream.next_out = again.data();
  stream.avail_out = static_cast<uInt>(again.size());
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP);
  EXPECT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(again.data(), input, input_length), 0);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}
#endif  // USE_IGZIP

// How an application actually reaches a post-Z_STREAM_END state: several
// members concatenated in one buffer, decoded on one z_stream with inflateReset
// between them. The rows above cover what the gate answers; this covers what it
// must not answer. The terminal flag is sticky by design, so a reset that
// failed to clear it -- or a gate placed above the reset entry points -- would
// leave every member after the first answered from the terminal state instead
// of decoded, and the stream would report success while returning nothing.
//
// Each member is fed as one call over the remaining bytes rather than as its
// own exact byte range, so what advances next_in between members is the decode
// itself, the way a caller reading from a stream would have it. Except on IAA:
// that path reports the whole buffer consumed on a stream that ends before the
// end of its input (the standing TODO in UncompressIAA -- QPL's consumed count
// is not usable at end of stream), so next_in cannot locate the next member
// there and each member is fed its exact range instead. What is under test
// either way is that the reset clears the terminal state.
static void RunConcatenatedMembersDecodeOverReset(ExecutionPath accel_path,
                                                  int window_bits,
                                                  uint32_t seed) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);
  const bool feed_exact_ranges = (accel_path == IAA);

  const size_t member_length = 32 * 1024;
  const int member_count = 3;
  std::vector<char*> members;
  std::vector<size_t> member_sizes;
  std::string concatenated;
  for (int i = 0; i < member_count; i++) {
    // Compressed by the same backend that will decode it, so the stream is one
    // it accepts -- the same reasoning as the inflate rows above.
    char* member = GenerateSeededCompressibleBlock(member_length, seed + i);
    ASSERT_NE(member, nullptr);
    members.push_back(member);

    std::string compressed;
    size_t output_upper_bound = 0;
    ExecutionPath compress_path = UNDEFINED;
    ASSERT_EQ(ZlibCompress(member, member_length, &compressed, window_bits,
                           Z_FINISH, &output_upper_bound, &compress_path),
              Z_STREAM_END);
    member_sizes.push_back(compressed.size());
    concatenated += compressed;
  }

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, window_bits), Z_OK);

  Bytef* next = reinterpret_cast<Bytef*>(&concatenated[0]);
  size_t remaining = concatenated.size();
  std::vector<Bytef> decoded(member_length + 1024);
  for (int i = 0; i < member_count; i++) {
    if (i > 0) {
      // The reset that clears the terminal state. Without it the calls below
      // are answered by the gate and nothing is decoded.
      ASSERT_EQ(inflateReset(&stream), Z_OK);
    }
    stream.next_in = next;
    stream.avail_in =
        static_cast<uInt>(feed_exact_ranges ? member_sizes[i] : remaining);
    stream.next_out = decoded.data();
    stream.avail_out = static_cast<uInt>(decoded.size());
    const uLong total_out_before = stream.total_out;

    int ret = Z_OK;
    for (int guard = 0; guard < 128; guard++) {
      ret = inflate(&stream, Z_NO_FLUSH);
      ASSERT_NE(ret, Z_DATA_ERROR) << "member " << i;
      if (ret == Z_STREAM_END) {
        break;
      }
    }
    ASSERT_EQ(ret, Z_STREAM_END) << "member " << i;
    EXPECT_EQ(GetInflateExecutionPath(&stream), accel_path) << "member " << i;
    // One member per decode: a call that ran past the member boundary would
    // leave the members after it undecodable, and zlib stops there too.
    EXPECT_EQ(stream.total_out - total_out_before, member_length)
        << "member " << i;
    EXPECT_EQ(memcmp(decoded.data(), members[i], member_length), 0)
        << "member " << i;

    if (feed_exact_ranges) {
      next += member_sizes[i];
      remaining -= member_sizes[i];
    } else {
      next = stream.next_in;
      remaining = stream.avail_in;
    }
  }
  EXPECT_EQ(remaining, 0u);

  // And the last member's terminal state is still reported, the concatenation
  // having changed nothing about the stream the gate sees.
  std::vector<Bytef> spare(4096);
  CheckInflatePostEndRows(&stream, concatenated, spare.data(), spare.size());

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  for (char* member : members) {
    DestroyBlock(member);
  }
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPConcatenatedZlibMembersDecode) {
  RunConcatenatedMembersDecodeOverReset(IGZIP, 15, 0x11e5);
}

TEST_F(TerminalStateRegressionTest, IGZIPConcatenatedGzipMembersDecode) {
  RunConcatenatedMembersDecodeOverReset(IGZIP, 31, 0x11e8);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATConcatenatedZlibMembersDecode) {
  RunConcatenatedMembersDecodeOverReset(QAT, 15, 0x11eb);
}

TEST_F(TerminalStateRegressionTest, QATConcatenatedGzipMembersDecode) {
  RunConcatenatedMembersDecodeOverReset(QAT, 31, 0x11ee);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAAConcatenatedZlibMembersDecode) {
  RunConcatenatedMembersDecodeOverReset(IAA, 15, 0x11f1);
}

TEST_F(TerminalStateRegressionTest, IAAConcatenatedGzipMembersDecode) {
  RunConcatenatedMembersDecodeOverReset(IAA, 31, 0x11f4);
}
#endif

TEST_F(TerminalStateRegressionTest, ZlibPathConcatenatedZlibMembersDecode) {
  RunConcatenatedMembersDecodeOverReset(ZLIB, 15, 0x11f7);
}

TEST_F(TerminalStateRegressionTest, ZlibPathConcatenatedGzipMembersDecode) {
  RunConcatenatedMembersDecodeOverReset(ZLIB, 31, 0x11fa);
}

// The parity guard: on a stream that never left zlib, every row above is
// answered by zlib itself. It passes both before and after this change, which
// is the point -- it is what makes the accelerator rows meaningful, and it
// fails if the gate ever starts answering for ZLIB-path streams with anything
// other than zlib's own returns.
TEST_F(TerminalStateRegressionTest, ZlibPathPostEndIsUnchanged) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 * 1024;
  char* input = GenerateBlock(input_length, compressible_block);
  ASSERT_NE(input, nullptr);

  z_stream cstream;
  memset(&cstream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  const size_t bound = deflateBound(&cstream, input_length) + 4096;
  std::vector<Bytef> output(bound);
  cstream.next_in = reinterpret_cast<Bytef*>(input);
  cstream.avail_in = static_cast<uInt>(input_length);
  cstream.next_out = output.data();
  cstream.avail_out = static_cast<uInt>(output.size());
  ASSERT_EQ(deflate(&cstream, Z_FINISH), Z_STREAM_END);
  ASSERT_EQ(GetDeflateExecutionPath(&cstream), ZLIB);
  std::string compressed(reinterpret_cast<char*>(output.data()),
                         cstream.total_out);

  std::vector<Bytef> spare(4096);
  CheckDeflatePostEndRows(&cstream, input, input_length, spare.data(),
                          spare.size());
  ASSERT_EQ(deflateEnd(&cstream), Z_OK);

  z_stream dstream;
  memset(&dstream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&dstream, 15), Z_OK);

  std::vector<Bytef> uncompressed(input_length + 1024);
  dstream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  dstream.avail_in = static_cast<uInt>(compressed.size());
  dstream.next_out = uncompressed.data();
  dstream.avail_out = static_cast<uInt>(uncompressed.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&dstream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&dstream), ZLIB);
  EXPECT_EQ(memcmp(uncompressed.data(), input, input_length), 0);

  CheckInflatePostEndRows(&dstream, compressed, spare.data(), spare.size());
  ASSERT_EQ(inflateEnd(&dstream), Z_OK);

  DestroyBlock(input);
}

// The null next_in and null next_out rows above are answered by the terminal
// state, so they say nothing about the same pointers on a stream that has not
// ended. Those reach the shim's own reads -- the block-header diagnostic, the
// zlib-header FDICT probe, the IAA decompressibility probe, and the offload
// itself -- all of which run before zlib gets to reject the pointer. Every row
// has to come back with zlib's Z_STREAM_ERROR and no bytes moved.
// Each row varies exactly one pointer: the null-next_out row keeps the input
// side valid rather than zeroing it too, so a failure names the pointer that
// caused it. Delegating these to zlib is also what makes msg match the
// terminal-state gate on the same pointers -- deflate() sets "stream error"
// through ERR_RETURN where inflate() leaves msg untouched, on a live stream and
// a finished one alike.
static void CheckNullPointersRejected(z_streamp stream, bool is_deflate,
                                      Bytef* buffer, size_t buffer_length,
                                      int flush) {
  const uLong total_in_before = stream->total_in;
  const uLong total_out_before = stream->total_out;
  const uInt length = static_cast<uInt>(buffer_length);

  struct NullPointerRow {
    const char* label;
    Bytef* next_in;
    uInt avail_in;
    Bytef* next_out;
    uInt avail_out;
  };
  const NullPointerRow rows[] = {
      {"next_in", nullptr, length, buffer, length},
      {"next_out", buffer, length, nullptr, length},
  };

  for (const NullPointerRow& row : rows) {
    stream->next_in = row.next_in;
    stream->avail_in = row.avail_in;
    stream->next_out = row.next_out;
    stream->avail_out = row.avail_out;
    stream->msg = const_cast<char*>(kMsgSentinel);

    const int ret =
        is_deflate ? deflate(stream, flush) : inflate(stream, flush);
    EXPECT_EQ(ret, Z_STREAM_ERROR) << row.label;
    EXPECT_EQ(stream->total_in, total_in_before) << row.label;
    EXPECT_EQ(stream->total_out, total_out_before) << row.label;
    EXPECT_STREQ(stream->msg, is_deflate ? "stream error" : kMsgSentinel)
        << row.label;
  }

  stream->msg = nullptr;
}

// log_level is a runtime config, so raising it is what exercises the
// block-header diagnostic on a library built with DEBUG_LOG.
static void RunInflateRejectsNullPointers(ExecutionPath accel_path) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/true, false);
  const uint32_t saved_log_level = GetConfig(LOG_LEVEL);
  SetConfig(LOG_LEVEL, static_cast<uint32_t>(LogLevel::LOG_INFO));

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> buffer(4096);
  CheckNullPointersRejected(&stream, /*is_deflate=*/false, buffer.data(),
                            buffer.size(), Z_NO_FLUSH);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  SetConfig(LOG_LEVEL, saved_log_level);
}

static void RunDeflateRejectsNullPointers(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(ZLIB, false, false);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  // Z_FINISH, so QAT and IAA consider the call offloadable and would hand the
  // null pointer to the vendor library.
  std::vector<Bytef> buffer(4096);
  CheckNullPointersRejected(&stream, /*is_deflate=*/true, buffer.data(),
                            buffer.size(), Z_FINISH);

  ASSERT_EQ(deflateEnd(&stream), Z_OK);
}

TEST_F(TerminalStateRegressionTest, ZlibPathRejectsNullPointers) {
  RunDeflateRejectsNullPointers(ZLIB);
  RunInflateRejectsNullPointers(ZLIB);
}

#ifdef USE_IGZIP
TEST_F(TerminalStateRegressionTest, IGZIPRejectsNullPointers) {
  RunDeflateRejectsNullPointers(IGZIP);
  RunInflateRejectsNullPointers(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(TerminalStateRegressionTest, QATRejectsNullPointers) {
  RunDeflateRejectsNullPointers(QAT);
  RunInflateRejectsNullPointers(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(TerminalStateRegressionTest, IAARejectsNullPointers) {
  RunDeflateRejectsNullPointers(IAA);
  RunInflateRejectsNullPointers(IAA);
}
#endif
#endif  // USE_IGZIP || USE_QAT || USE_IAA

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
  EXPECT_EQ(gzclose_r(nullptr), Z_STREAM_ERROR);
  EXPECT_EQ(gzclose_w(nullptr), Z_STREAM_ERROR);
  EXPECT_EQ(gzsetparams(nullptr, 1, Z_DEFAULT_STRATEGY), Z_STREAM_ERROR);
  EXPECT_EQ(gzflush(nullptr, Z_SYNC_FLUSH), Z_STREAM_ERROR);
  EXPECT_EQ(gzputc(nullptr, 'a'), -1);
  EXPECT_EQ(gzputs(nullptr, "a"), -1);
  EXPECT_EQ(gzfwrite(buf.data(), 1, buf.size(), nullptr), 0u);
  EXPECT_EQ(gzprintf(nullptr, "%d", 1), Z_STREAM_ERROR);
  // zlib.h's gzgetc macro dereferences the handle, so a null one has to reach
  // the function form: gzgetc_, or the parenthesized name.
  EXPECT_EQ(gzgetc_(nullptr), -1);
  EXPECT_EQ((gzgetc)(nullptr), -1);
  EXPECT_EQ(gzungetc('a', nullptr), -1);
  EXPECT_EQ(gzgets(nullptr, reinterpret_cast<char*>(buf.data()),
                   static_cast<int>(buf.size())),
            nullptr);
  EXPECT_EQ(gzfread(buf.data(), 1, buf.size(), nullptr), 0u);
}

// deflatePrime() and inflatePrime() write bits into zlib's own bit buffer. No
// backend has one the shim can reach, so before these two were intercepted the
// call went straight to zlib and the bits were then dropped by the engine that
// actually ran the stream: a primed byte that never appeared in deflate output,
// and a decoder that was asked to start off a byte boundary reporting a clean
// Z_STREAM_END on bytes zlib refuses. The fix pins a primed stream to zlib and
// refuses the call outright once an accelerator holds the stream.
class PrimeRegressionTest : public ::testing::Test {
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
    // Written unconditionally by SetCompressPath/SetUncompressPath as well.
    saved_iaa_prepend_empty_block_ = GetConfig(IAA_PREPEND_EMPTY_BLOCK);
    saved_qat_allow_chunking_ = GetConfig(QAT_COMPRESSION_ALLOW_CHUNKING);
    saved_igzip_fallback_ = GetConfig(IGZIP_FALLBACK);
  }

  // Restored here rather than at the end of each helper: an ASSERT_* failure
  // returns from the helper, which would skip an inline restore and leave the
  // rest of the suite running on this test's configuration.
  void TearDown() override {
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
    SetConfig(IGZIP_FALLBACK, saved_igzip_fallback_);
  }

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
  uint32_t saved_igzip_fallback_ = 0;
};

// Outside the accelerator guard below: a null stream reaches neither the
// registry nor an engine, so this covers the wrappers' own ordering in the
// build CI runs, which has every backend off.
TEST_F(PrimeRegressionTest, PrimeRejectsANullStream) {
  EXPECT_EQ(deflatePrime(nullptr, 8, 0xA5), Z_STREAM_ERROR);
  EXPECT_EQ(inflatePrime(nullptr, 8, 0xA5), Z_STREAM_ERROR);
}

#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)

namespace {

constexpr size_t kPrimeInputLength = 64 * 1024;
// An arbitrary byte with bits in both nibbles, so a stream that dropped it
// cannot coincide with one that kept it.
constexpr int kPrimedByte = 0xA5;

}  // namespace

// A raw stream, because on the zlib format the two header bytes are written
// straight to the pending buffer and the primed bits land behind them; with
// windowBits negative the bit buffer is the first thing flushed, so the primed
// byte is the stream's first byte and its survival is directly observable.
static void RunDeflatePrimeKeepsPrimedBits(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(ZLIB, false, false);

  char* input = GenerateSeededCompressibleBlock(kPrimeInputLength,
                                                /*seed=*/0x9c31);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  ASSERT_EQ(deflatePrime(&stream, 8, kPrimedByte), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB)
      << "a primed stream must be pinned to the only engine whose bit buffer "
         "the call reached";

  std::vector<Bytef> output(deflateBound(&stream, kPrimeInputLength) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(kPrimeInputLength);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);
  ASSERT_GT(stream.total_out, 1u);
  output.resize(stream.total_out);
  ASSERT_EQ(deflateEnd(&stream), Z_OK);

  EXPECT_EQ(output[0], kPrimedByte)
      << "the caller's bits must appear ahead of the deflate stream";

  // What follows the primed byte has to be an intact raw deflate stream, or the
  // bits were kept at the cost of the payload.
  z_stream decode;
  memset(&decode, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&decode, -15), Z_OK);
  std::vector<Bytef> decoded(kPrimeInputLength + 1024);
  decode.next_in = output.data() + 1;
  decode.avail_in = static_cast<uInt>(output.size() - 1);
  decode.next_out = decoded.data();
  decode.avail_out = static_cast<uInt>(decoded.size());
  EXPECT_EQ(inflate(&decode, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(decode.total_out, kPrimeInputLength);
  EXPECT_EQ(memcmp(decoded.data(), input, kPrimeInputLength), 0);
  ASSERT_EQ(inflateEnd(&decode), Z_OK);

  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(PrimeRegressionTest, IGZIPDeflatePrimeKeepsPrimedBits) {
  RunDeflatePrimeKeepsPrimedBits(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(PrimeRegressionTest, QATDeflatePrimeKeepsPrimedBits) {
  RunDeflatePrimeKeepsPrimedBits(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(PrimeRegressionTest, IAADeflatePrimeKeepsPrimedBits) {
  RunDeflatePrimeKeepsPrimedBits(IAA);
}
#endif

// The pin costs the whole stream its offload, so it must be narrow: a call that
// primes nothing, and a call zlib rejects without touching its bit buffer, both
// leave the stream exactly as offloadable as they found it.
static void RunDeflatePrimeWithoutBitsKeepsOffload(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  char* input = GenerateSeededCompressibleBlock(kPrimeInputLength,
                                                /*seed=*/0x9c32);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  EXPECT_EQ(deflatePrime(&stream, 0, 0), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), UNDEFINED);
  // zlib's own refusals for a bit count it cannot hold. Neither writes a bit,
  // so neither is a reason to give up the accelerator.
  EXPECT_EQ(deflatePrime(&stream, 17, 0), Z_BUF_ERROR);
  EXPECT_EQ(deflatePrime(&stream, -1, 0), Z_BUF_ERROR);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), UNDEFINED);

  std::vector<Bytef> output(deflateBound(&stream, kPrimeInputLength) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(kPrimeInputLength);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), accel_path);
  output.resize(stream.total_out);
  ASSERT_EQ(deflateEnd(&stream), Z_OK);

  z_stream decode;
  memset(&decode, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&decode, 15), Z_OK);
  std::vector<Bytef> decoded(kPrimeInputLength + 1024);
  decode.next_in = output.data();
  decode.avail_in = static_cast<uInt>(output.size());
  decode.next_out = decoded.data();
  decode.avail_out = static_cast<uInt>(decoded.size());
  EXPECT_EQ(inflate(&decode, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(decode.total_out, kPrimeInputLength);
  EXPECT_EQ(memcmp(decoded.data(), input, kPrimeInputLength), 0);
  ASSERT_EQ(inflateEnd(&decode), Z_OK);

  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(PrimeRegressionTest, IGZIPDeflatePrimeWithoutBitsKeepsOffload) {
  RunDeflatePrimeWithoutBitsKeepsOffload(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(PrimeRegressionTest, QATDeflatePrimeWithoutBitsKeepsOffload) {
  RunDeflatePrimeWithoutBitsKeepsOffload(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(PrimeRegressionTest, IAADeflatePrimeWithoutBitsKeepsOffload) {
  RunDeflatePrimeWithoutBitsKeepsOffload(IAA);
}
#endif

// Once the accelerator has produced output there is nowhere to put the bits:
// zlib's deflate state never saw the stream, so it can neither emit them where
// the caller asked nor continue from where the accelerator left off. zlib
// itself accepts the call, so the refusal is a deliberate divergence -- a
// return code the caller can act on, in place of bits that quietly disappear.
static void RunDeflatePrimeRefusedOnAcceleratedStream(
    ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/false, false, false);
  SetUncompressPath(ZLIB, false, false);

  char* input = GenerateSeededCompressibleBlock(kPrimeInputLength,
                                                /*seed=*/0x9c33);
  ASSERT_NE(input, nullptr);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> output(deflateBound(&stream, kPrimeInputLength) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(input);
  stream.avail_in = static_cast<uInt>(kPrimeInputLength);
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  ASSERT_EQ(GetDeflateExecutionPath(&stream), accel_path);

  EXPECT_EQ(deflatePrime(&stream, 8, kPrimedByte), Z_STREAM_ERROR);
  // The refusal is about bits, not about the call: priming nothing asks for
  // nothing and is passed through.
  EXPECT_EQ(deflatePrime(&stream, 0, 0), Z_OK);
  // A count zlib refuses primes nothing either, so it keeps zlib's own answer
  // here exactly as it does on a stream no engine has taken yet.
  EXPECT_EQ(deflatePrime(&stream, 17, 0), Z_BUF_ERROR);
  EXPECT_EQ(deflatePrime(&stream, -1, 0), Z_BUF_ERROR);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), accel_path)
      << "a refused prime must not take the stream off its engine either";

  // A reset discards zlib's bit buffer, so there is nothing primed left to
  // honor and the next stream is offloadable again.
  ASSERT_EQ(deflateReset(&stream), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), UNDEFINED);
  EXPECT_EQ(deflatePrime(&stream, 8, kPrimedByte), Z_OK);
  EXPECT_EQ(GetDeflateExecutionPath(&stream), ZLIB);

  ASSERT_EQ(deflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(PrimeRegressionTest, IGZIPDeflatePrimeRefusedOnAcceleratedStream) {
  RunDeflatePrimeRefusedOnAcceleratedStream(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(PrimeRegressionTest, QATDeflatePrimeRefusedOnAcceleratedStream) {
  RunDeflatePrimeRefusedOnAcceleratedStream(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(PrimeRegressionTest, IAADeflatePrimeRefusedOnAcceleratedStream) {
  RunDeflatePrimeRefusedOnAcceleratedStream(IAA);
}
#endif

// The decode side is the one that can report success on data zlib refuses.
// Priming the first byte and feeding the rest is the standard way to resume a
// stream that does not start on a byte boundary; an engine that ignores the
// primed bits sees a stream shifted by one byte, and on a raw stream it happily
// decodes the shift into junk. Pinned to zlib, the shifted feed decodes to
// exactly the original payload.
static void RunInflatePrimeDecodesAShiftedStream(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/true, false);

  char* input = GenerateSeededCompressibleBlock(kPrimeInputLength,
                                                /*seed=*/0x9c34);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, kPrimeInputLength, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);
  ASSERT_GT(compressed.size(), 1u);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  ASSERT_EQ(inflatePrime(&stream, 8, static_cast<unsigned char>(compressed[0])),
            Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);

  std::vector<Bytef> decoded(kPrimeInputLength + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[1]);
  stream.avail_in = static_cast<uInt>(compressed.size() - 1);
  stream.next_out = decoded.data();
  stream.avail_out = static_cast<uInt>(decoded.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  EXPECT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);
  EXPECT_EQ(stream.total_out, kPrimeInputLength);
  EXPECT_EQ(memcmp(decoded.data(), input, kPrimeInputLength), 0);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(PrimeRegressionTest, IGZIPInflatePrimeDecodesAShiftedStream) {
  RunInflatePrimeDecodesAShiftedStream(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(PrimeRegressionTest, QATInflatePrimeDecodesAShiftedStream) {
  RunInflatePrimeDecodesAShiftedStream(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(PrimeRegressionTest, IAAInflatePrimeDecodesAShiftedStream) {
  RunInflatePrimeDecodesAShiftedStream(IAA);
}
#endif

// The decode-side counterpart of RunDeflatePrimeWithoutBitsKeepsOffload. The
// negative form is included here rather than among the refusals: before the
// first inflate() call neither zlib nor an engine holds a bit, so discarding
// what is held changes nothing and must not cost the offload.
static void RunInflatePrimeWithoutBitsKeepsOffload(ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);

  char* input = GenerateSeededCompressibleBlock(kPrimeInputLength,
                                                /*seed=*/0x9c35);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, kPrimeInputLength, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  EXPECT_EQ(inflatePrime(&stream, 0, 0), Z_OK);
  EXPECT_EQ(inflatePrime(&stream, -1, 0), Z_OK);
  // zlib's own refusal for a bit count it cannot hold.
  EXPECT_EQ(inflatePrime(&stream, 17, 0), Z_STREAM_ERROR);
  EXPECT_EQ(GetInflateExecutionPath(&stream), UNDEFINED);

  std::vector<Bytef> decoded(kPrimeInputLength + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = decoded.data();
  stream.avail_out = static_cast<uInt>(decoded.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  EXPECT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), accel_path);
  EXPECT_EQ(stream.total_out, kPrimeInputLength);
  EXPECT_EQ(memcmp(decoded.data(), input, kPrimeInputLength), 0);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(PrimeRegressionTest, IGZIPInflatePrimeWithoutBitsKeepsOffload) {
  RunInflatePrimeWithoutBitsKeepsOffload(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(PrimeRegressionTest, QATInflatePrimeWithoutBitsKeepsOffload) {
  RunInflatePrimeWithoutBitsKeepsOffload(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(PrimeRegressionTest, IAAInflatePrimeWithoutBitsKeepsOffload) {
  RunInflatePrimeWithoutBitsKeepsOffload(IAA);
}
#endif

// Same refusal as the deflate side, for the same reason: the engine holds the
// bits the call would have to modify, and zlib's inflate state never saw the
// stream. Both the priming and the discarding form are refused; a reset returns
// the stream to path selection with nothing primed on either side.
static void RunInflatePrimeRefusedOnAcceleratedStream(
    ExecutionPath accel_path) {
  SetCompressPath(accel_path, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/false, false);

  char* input = GenerateSeededCompressibleBlock(kPrimeInputLength,
                                                /*seed=*/0x9c36);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, kPrimeInputLength, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<Bytef> decoded(kPrimeInputLength + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = decoded.data();
  stream.avail_out = static_cast<uInt>(decoded.size());
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  ASSERT_EQ(ret, Z_STREAM_END);
  ASSERT_EQ(GetInflateExecutionPath(&stream), accel_path);

  EXPECT_EQ(inflatePrime(&stream, 8, kPrimedByte), Z_STREAM_ERROR);
  EXPECT_EQ(inflatePrime(&stream, -1, 0), Z_STREAM_ERROR);
  EXPECT_EQ(inflatePrime(&stream, 0, 0), Z_OK);
  // zlib refuses an over-16 count with the same code, so this row is here to
  // record that the answer does not depend on which side produced it.
  EXPECT_EQ(inflatePrime(&stream, 17, 0), Z_STREAM_ERROR);
  EXPECT_EQ(GetInflateExecutionPath(&stream), accel_path);

  ASSERT_EQ(inflateReset(&stream), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), UNDEFINED);
  EXPECT_EQ(inflatePrime(&stream, 8, kPrimedByte), Z_OK);
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

#ifdef USE_IGZIP
TEST_F(PrimeRegressionTest, IGZIPInflatePrimeRefusedOnAcceleratedStream) {
  RunInflatePrimeRefusedOnAcceleratedStream(IGZIP);
}
#endif

#ifdef USE_QAT
TEST_F(PrimeRegressionTest, QATInflatePrimeRefusedOnAcceleratedStream) {
  RunInflatePrimeRefusedOnAcceleratedStream(QAT);
}
#endif

#ifdef USE_IAA
TEST_F(PrimeRegressionTest, IAAInflatePrimeRefusedOnAcceleratedStream) {
  RunInflatePrimeRefusedOnAcceleratedStream(IAA);
}
#endif

#ifdef USE_IGZIP
// IGZIP is the only backend that holds a stream part-way through, so it is the
// only one where the refusal covers a stream that is neither finished nor
// restartable. QAT and IAA never reach this state: a decode that does not end
// in one call falls back to zlib and the stream is on the zlib path from there.
TEST_F(PrimeRegressionTest, IGZIPInflatePrimeRefusedMidStream) {
  SetCompressPath(IGZIP, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(IGZIP, /*zlib_fallback=*/false, false);

  char* input = GenerateSeededCompressibleBlock(kPrimeInputLength,
                                                /*seed=*/0x9c37);
  ASSERT_NE(input, nullptr);

  std::string compressed;
  size_t output_upper_bound = 0;
  ExecutionPath compress_path = UNDEFINED;
  ASSERT_EQ(ZlibCompress(input, kPrimeInputLength, &compressed, 15, Z_FINISH,
                         &output_upper_bound, &compress_path),
            Z_STREAM_END);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  // Half the input, so the stream is mid-block when the prime arrives.
  std::vector<Bytef> decoded(kPrimeInputLength + 1024);
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]);
  stream.avail_in = static_cast<uInt>(compressed.size() / 2);
  stream.next_out = decoded.data();
  stream.avail_out = static_cast<uInt>(decoded.size());
  ASSERT_EQ(inflate(&stream, Z_NO_FLUSH), Z_OK);
  ASSERT_EQ(GetInflateExecutionPath(&stream), IGZIP);
  ASSERT_GT(stream.total_in, 0u);

  EXPECT_EQ(inflatePrime(&stream, 8, kPrimedByte), Z_STREAM_ERROR);

  // The refusal leaves the stream running: the rest of the input still decodes
  // on IGZIP to the original payload.
  stream.next_in = reinterpret_cast<Bytef*>(&compressed[0]) + stream.total_in;
  stream.avail_in = static_cast<uInt>(compressed.size() - stream.total_in);
  int ret = Z_OK;
  for (int guard = 0; guard < 128; guard++) {
    ret = inflate(&stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END) {
      break;
    }
  }
  EXPECT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(GetInflateExecutionPath(&stream), IGZIP);
  EXPECT_EQ(stream.total_out, kPrimeInputLength);
  EXPECT_EQ(memcmp(decoded.data(), input, kPrimeInputLength), 0);

  ASSERT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}
#endif  // USE_IGZIP
#endif  // USE_IGZIP || USE_QAT || USE_IAA

#ifdef USE_IGZIP
class InflateMidstreamErrorRegressionTest : public ::testing::Test {
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
    // Written unconditionally by SetCompressPath/SetUncompressPath as well.
    saved_iaa_prepend_empty_block_ = GetConfig(IAA_PREPEND_EMPTY_BLOCK);
    saved_qat_allow_chunking_ = GetConfig(QAT_COMPRESSION_ALLOW_CHUNKING);
    saved_igzip_fallback_ = GetConfig(IGZIP_FALLBACK);
    saved_ignore_dictionary_ = GetConfig(IGNORE_ZLIB_DICTIONARY);
  }

  // Restored here rather than at the end of each helper: an ASSERT_* failure
  // returns from the helper, which would skip an inline restore and leave the
  // rest of the suite running on this test's configuration.
  void TearDown() override {
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
    SetConfig(IGZIP_FALLBACK, saved_igzip_fallback_);
    SetConfig(IGNORE_ZLIB_DICTIONARY, saved_ignore_dictionary_);
  }

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
  uint32_t saved_igzip_fallback_ = 0;
  uint32_t saved_ignore_dictionary_ = 0;
};

// Build a stream that decodes cleanly up to a point and is invalid after it.
// Z_SYNC_FLUSH ends the good part at a byte boundary with the stream still
// open, and the byte appended there opens a block whose type is the reserved
// 11b, which every conforming decoder must reject. The whole payload is
// decodable from the prefix, which is what makes the delivered byte count
// assertable.
static void BuildStreamWithInvalidTail(const char* input, size_t input_length,
                                       int window_bits,
                                       std::vector<Bytef>* stream_out) {
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         window_bits, 8, Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> buffer(deflateBound(&stream, input_length) + 4096);
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input));
  stream.avail_in = static_cast<uInt>(input_length);
  stream.next_out = buffer.data();
  stream.avail_out = static_cast<uInt>(buffer.size());

  ASSERT_EQ(deflate(&stream, Z_SYNC_FLUSH), Z_OK);
  ASSERT_EQ(stream.avail_in, 0u);
  const size_t prefix_length = buffer.size() - stream.avail_out;
  // Z_DATA_ERROR, not Z_OK: the stream is deliberately left unfinished, which
  // is what deflateEnd() reports when it is ended before Z_FINISH.
  ASSERT_EQ(deflateEnd(&stream), Z_DATA_ERROR);

  buffer.resize(prefix_length + 64);
  buffer[prefix_length] = 0x07;
  for (size_t i = prefix_length + 1; i < buffer.size(); i++) {
    buffer[i] = 0x5a;
  }
  *stream_out = buffer;
}

// Feed a stream to inflate() in fixed-size chunks, stopping at the first error
// or at the end of the stream. Reports what the caller needs to judge the
// failure: whether a completion was ever claimed, and what the last call said.
// Resumable: the cursor comes from total_in, so a caller that resynchronized
// the stream in between continues from where the resync left it.
static void InflateInChunks(z_streamp stream, const std::vector<Bytef>& input,
                            size_t chunk_length, int* last_ret,
                            bool* saw_stream_end) {
  *last_ret = Z_OK;
  *saw_stream_end = false;
  size_t fed = stream->total_in;
  stream->avail_in = 0;
  for (int guard = 0; guard < 1024; guard++) {
    if (stream->avail_in == 0 && fed < input.size()) {
      const size_t take = std::min(chunk_length, input.size() - fed);
      stream->next_in = const_cast<Bytef*>(input.data()) + fed;
      stream->avail_in = static_cast<uInt>(take);
      fed += take;
    }
    *last_ret = inflate(stream, Z_NO_FLUSH);
    if (*last_ret == Z_STREAM_END) {
      *saw_stream_end = true;
      return;
    }
    if (*last_ret < 0) {
      return;
    }
    if (stream->avail_in == 0 && fed >= input.size() &&
        *last_ret == Z_BUF_ERROR) {
      return;
    }
  }
  FAIL() << "inflate() made no progress in 1024 calls";
}

// The finding: a decode failure part-way through a stream used to pin the
// stream to zlib and hand it the rest, but zlib's inflate state never saw the
// earlier chunks, so next_in pointed into the middle of a deflate block that
// zlib read as the start of a fresh stream. On a raw stream there is no header
// to reject that, so zlib emitted junk it counted as output and could report
// Z_STREAM_END -- a short decode reported as success -- and the output the
// failing call had already decoded was dropped on the way out.
static void RunMidstreamInflateErrorRegression(ExecutionPath accel_path,
                                               int window_bits, uint32_t seed) {
  const size_t input_length = 96 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, seed);
  ASSERT_NE(input, nullptr);

  std::vector<Bytef> compressed;
  BuildStreamWithInvalidTail(input, input_length, window_bits, &compressed);
  ASSERT_FALSE(compressed.empty());

  SetUncompressPath(accel_path, /*zlib_fallback=*/true, false);
  if (accel_path != IGZIP) {
    SetConfig(USE_IGZIP_UNCOMPRESS, 1);
    SetConfig(IGZIP_FALLBACK, 1);
  }

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, window_bits), Z_OK);

  std::vector<char> output(input_length + 4096, 0);
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());

  int last_ret = Z_OK;
  bool saw_stream_end = false;
  // Chunked deliberately: one call per stream is the case the fall-through was
  // written for, and it stays supported -- see the first-call test below.
  InflateInChunks(&stream, compressed, 4096, &last_ret, &saw_stream_end);

  // The stream is broken, so the one answer that must never appear is success.
  EXPECT_FALSE(saw_stream_end);
  EXPECT_EQ(last_ret, Z_DATA_ERROR);

  // Everything before the invalid block is decodable, and an engine that
  // decoded it has to hand it over -- zlib returns the decodable prefix and
  // then the error too.
  const size_t produced = output.size() - stream.avail_out;
  EXPECT_EQ(produced, input_length);
  EXPECT_EQ(memcmp(output.data(), input, std::min(produced, input_length)), 0);

  // Answered in place rather than delegated: a stream this far in cannot be
  // handed to zlib at all, so the path must not have moved to ZLIB.
  EXPECT_NE(GetInflateExecutionPath(&stream), ZLIB);

  // A failed stream stays failed. zlib holds its own state at BAD and keeps
  // answering Z_DATA_ERROR; the engine here has no such state of its own.
  stream.next_in = compressed.data();
  stream.avail_in = static_cast<uInt>(compressed.size());
  EXPECT_EQ(inflate(&stream, Z_NO_FLUSH), Z_DATA_ERROR);

  EXPECT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

// The other side of the same gate. When the very first call fails, next_in
// still addresses the first byte of the stream, so zlib can take it from the
// start -- which is what the fall-through was for, and it has to keep working.
static void RunFirstCallInflateErrorRegression(ExecutionPath accel_path,
                                               int window_bits, uint32_t seed) {
  const size_t input_length = 32 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, seed);
  ASSERT_NE(input, nullptr);

  std::vector<Bytef> compressed;
  BuildStreamWithInvalidTail(input, input_length, window_bits, &compressed);
  ASSERT_FALSE(compressed.empty());
  // Smash the first block instead of the appended one, leaving the zlib header
  // valid so the stream is refused for its content rather than its format.
  const size_t header_length = (window_bits >= 8) ? 2 : 0;
  ASSERT_GT(compressed.size(), header_length);
  compressed[header_length] = 0x07;

  SetUncompressPath(accel_path, /*zlib_fallback=*/true, false);
  if (accel_path != IGZIP) {
    SetConfig(USE_IGZIP_UNCOMPRESS, 1);
    SetConfig(IGZIP_FALLBACK, 1);
  }

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, window_bits), Z_OK);

  std::vector<char> output(input_length + 4096, 0);
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());

  int last_ret = Z_OK;
  bool saw_stream_end = false;
  InflateInChunks(&stream, compressed, 4096, &last_ret, &saw_stream_end);

  EXPECT_FALSE(saw_stream_end);
  EXPECT_EQ(last_ret, Z_DATA_ERROR);
  EXPECT_EQ(output.size() - stream.avail_out, 0u);
  // Nothing was consumed, so this is the one case zlib can still be given.
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);

  EXPECT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

// inflateSync() is the way back from a failed stream: it skips to the next
// full-flush point and decoding resumes there. Recovery has to survive the
// mid-stream gate above -- the resync clears the latched error, or a stream
// that resynchronized successfully would answer Z_DATA_ERROR forever.
static void RunInflateSyncAfterMidstreamErrorRegression(
    ExecutionPath accel_path, int window_bits, uint32_t seed) {
  const size_t segment_length = 32 * 1024;
  const size_t input_length = 3 * segment_length;
  char* input = GenerateSeededCompressibleBlock(input_length, seed);
  ASSERT_NE(input, nullptr);

  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);

  z_stream cstream;
  memset(&cstream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         window_bits, 8, Z_DEFAULT_STRATEGY),
            Z_OK);

  std::vector<Bytef> compressed(deflateBound(&cstream, input_length) + 4096);
  cstream.next_out = compressed.data();
  cstream.avail_out = static_cast<uInt>(compressed.size());

  // Z_FULL_FLUSH discards the window, so each segment decodes from its own
  // start. That is what a resync can recover to.
  size_t mark[3] = {0, 0, 0};
  for (int segment = 0; segment < 3; segment++) {
    cstream.next_in =
        reinterpret_cast<Bytef*>(input) + segment * segment_length;
    cstream.avail_in = static_cast<uInt>(segment_length);
    const int ret = deflate(&cstream, segment == 2 ? Z_FINISH : Z_FULL_FLUSH);
    ASSERT_EQ(ret, segment == 2 ? Z_STREAM_END : Z_OK);
    mark[segment] = compressed.size() - cstream.avail_out;
  }
  const size_t compressed_length = compressed.size() - cstream.avail_out;
  ASSERT_EQ(deflateEnd(&cstream), Z_OK);
  compressed.resize(compressed_length);

  // Smash the middle segment from its byte-aligned start, keeping its trailing
  // flush marker so the resync still has a point to find.
  compressed[mark[0]] = 0x07;
  for (size_t i = mark[0] + 1; i + 8 < mark[1]; i++) {
    compressed[i] = 0x5a;
  }

  SetUncompressPath(accel_path, /*zlib_fallback=*/true, false);
  if (accel_path != IGZIP) {
    SetConfig(USE_IGZIP_UNCOMPRESS, 1);
    SetConfig(IGZIP_FALLBACK, 1);
  }

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, window_bits), Z_OK);

  std::vector<char> output(input_length + 4096, 0);
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());

  int last_ret = Z_OK;
  bool saw_stream_end = false;
  InflateInChunks(&stream, compressed, 4096, &last_ret, &saw_stream_end);
  ASSERT_EQ(last_ret, Z_DATA_ERROR);
  const size_t produced_before_sync = output.size() - stream.avail_out;
  ASSERT_GE(produced_before_sync, segment_length);
  EXPECT_EQ(memcmp(output.data(), input, segment_length), 0);

  // inflateSync() searches the input it is given, and the flush point it needs
  // is past what the failing call had been fed.
  stream.next_in = compressed.data() + stream.total_in;
  stream.avail_in = static_cast<uInt>(compressed.size() - stream.total_in);
  ASSERT_EQ(inflateSync(&stream), Z_OK);

  // Segment 3 comes back, which it cannot if the resync left the error latched.
  InflateInChunks(&stream, compressed, 4096, &last_ret, &saw_stream_end);
  EXPECT_NE(last_ret, Z_DATA_ERROR);
  const size_t produced = output.size() - stream.avail_out;
  ASSERT_GE(produced, segment_length);
  EXPECT_EQ(memcmp(output.data() + produced - segment_length,
                   input + 2 * segment_length, segment_length),
            0);
  // The resync is the one mid-stream handoff to zlib, and the only engine that
  // can honor it: zlib performed the search, so its state is the one left at
  // the flush point. A backend still holds the bits it read ahead of the
  // failure.
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);

  EXPECT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

// The other way a stream reaches the gate with input already consumed: a zlib
// header delivered one byte per call. FDICT lives in the second byte, so the
// first call is the shim's only chance to keep the stream off a backend, and it
// cannot yet see the bit. A backend given that byte swallows it into its own
// header buffer and reports the dictionary on the next call, by which time the
// bytes zlib would need to parse the header itself are gone from the caller's
// buffer -- so the request has to be answered the way zlib answers it, which
// means not offloading the byte at all.
static void RunFragmentedZlibHeaderDictionaryRegression(
    ExecutionPath accel_path, uint32_t seed) {
  SetCompressPath(ZLIB, /*zlib_fallback=*/true, false, false);
  SetUncompressPath(accel_path, /*zlib_fallback=*/true, false);
  SetConfig(IGNORE_ZLIB_DICTIONARY, 0);

  const size_t input_length = 32 * 1024;
  char* input = GenerateSeededCompressibleBlock(input_length, seed);
  ASSERT_NE(input, nullptr);

  const unsigned char dict[] = "fragmented-header-preset-dictionary";
  const uInt dict_length = static_cast<uInt>(sizeof(dict) - 1);

  z_stream cstream;
  memset(&cstream, 0, sizeof(z_stream));
  ASSERT_EQ(deflateInit2(&cstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
                         Z_DEFAULT_STRATEGY),
            Z_OK);
  ASSERT_EQ(deflateSetDictionary(&cstream, dict, dict_length), Z_OK);

  std::vector<Bytef> compressed(deflateBound(&cstream, input_length) + 4096);
  cstream.next_in = reinterpret_cast<Bytef*>(input);
  cstream.avail_in = static_cast<uInt>(input_length);
  cstream.next_out = compressed.data();
  cstream.avail_out = static_cast<uInt>(compressed.size());
  ASSERT_EQ(deflate(&cstream, Z_FINISH), Z_STREAM_END);
  compressed.resize(compressed.size() - cstream.avail_out);
  ASSERT_EQ(deflateEnd(&cstream), Z_OK);
  ASSERT_GT(compressed.size(), 2u);
  // FDICT, second header byte -- the bit the first call cannot see.
  ASSERT_NE(compressed[1] & 0x20, 0);

  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));
  ASSERT_EQ(inflateInit2(&stream, 15), Z_OK);

  std::vector<char> output(input_length + 4096, 0);
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());

  size_t fed = 0;
  int ret = Z_OK;
  bool asked_for_dictionary = false;
  for (size_t guard = 0; guard < 4 * compressed.size() + 64; guard++) {
    if (stream.avail_in == 0 && fed < compressed.size()) {
      stream.next_in = compressed.data() + fed;
      stream.avail_in = 1;
      fed++;
    }
    ret = inflate(&stream, Z_NO_FLUSH);
    if (ret == Z_NEED_DICT) {
      asked_for_dictionary = true;
      ASSERT_EQ(inflateSetDictionary(&stream, dict, dict_length), Z_OK);
      continue;
    }
    ASSERT_NE(ret, Z_DATA_ERROR);
    if (ret == Z_STREAM_END || ret == Z_BUF_ERROR) {
      break;
    }
  }

  // zlib asks for the dictionary and then decodes the stream; the pin is what
  // lets the shim do the same.
  EXPECT_TRUE(asked_for_dictionary);
  EXPECT_EQ(ret, Z_STREAM_END);
  EXPECT_EQ(stream.total_out, input_length);
  EXPECT_EQ(memcmp(output.data(), input, input_length), 0);
  EXPECT_EQ(GetInflateExecutionPath(&stream), ZLIB);

  EXPECT_EQ(inflateEnd(&stream), Z_OK);
  DestroyBlock(input);
}

TEST_F(InflateMidstreamErrorRegressionTest, IGZIPRawErrorIsNotReportedAsEnd) {
  RunMidstreamInflateErrorRegression(IGZIP, -15, /*seed=*/0x11f4);
}

TEST_F(InflateMidstreamErrorRegressionTest,
       IGZIPZlibErrorDeliversDecodedBytes) {
  RunMidstreamInflateErrorRegression(IGZIP, 15, /*seed=*/0x11f5);
}

TEST_F(InflateMidstreamErrorRegressionTest,
       IGZIPGzipErrorDeliversDecodedBytes) {
  RunMidstreamInflateErrorRegression(IGZIP, 31, /*seed=*/0x11f6);
}

TEST_F(InflateMidstreamErrorRegressionTest,
       IGZIPFirstCallErrorStillReachesZlib) {
  RunFirstCallInflateErrorRegression(IGZIP, 15, /*seed=*/0x11f7);
}

TEST_F(InflateMidstreamErrorRegressionTest,
       IGZIPInflateSyncRecoversAfterError) {
  RunInflateSyncAfterMidstreamErrorRegression(IGZIP, -15, /*seed=*/0x11f8);
}

TEST_F(InflateMidstreamErrorRegressionTest,
       IGZIPFragmentedZlibHeaderStillAsksForDictionary) {
  RunFragmentedZlibHeaderDictionaryRegression(IGZIP, /*seed=*/0x11fb);
}

#ifdef USE_QAT
TEST_F(InflateMidstreamErrorRegressionTest,
       QATFallbackRawErrorIsNotReportedAsEnd) {
  RunMidstreamInflateErrorRegression(QAT, -15, /*seed=*/0x11f9);
}
#endif

#ifdef USE_IAA
TEST_F(InflateMidstreamErrorRegressionTest,
       IAAFallbackRawErrorIsNotReportedAsEnd) {
  RunMidstreamInflateErrorRegression(IAA, -15, /*seed=*/0x11fa);
}
#endif
#endif  // USE_IGZIP
