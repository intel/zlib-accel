// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// The gz* file API, shim-owned and zlib-owned, including position and error
// state.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "../config/config.h"
#include "../zlib_accel.h"
#include "test_utils.h"

using namespace config;

// These tests mutate the global path configuration, so restore it in TearDown
// rather than at the end of the test body: ASSERT_* returns from the function,
// which would skip an inline restore and leak the setting into later tests.
class GzipFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_iaa_compress_ = GetConfig(USE_IAA_COMPRESS);
    saved_qat_compress_ = GetConfig(USE_QAT_COMPRESS);
    saved_igzip_compress_ = GetConfig(USE_IGZIP_COMPRESS);
    saved_zlib_compress_ = GetConfig(USE_ZLIB_COMPRESS);
    saved_iaa_uncompress_ = GetConfig(USE_IAA_UNCOMPRESS);
    saved_qat_uncompress_ = GetConfig(USE_QAT_UNCOMPRESS);
    saved_igzip_uncompress_ = GetConfig(USE_IGZIP_UNCOMPRESS);
    saved_zlib_uncompress_ = GetConfig(USE_ZLIB_UNCOMPRESS);
  }

  void TearDown() override {
    SetConfig(USE_IAA_COMPRESS, saved_iaa_compress_);
    SetConfig(USE_QAT_COMPRESS, saved_qat_compress_);
    SetConfig(USE_IGZIP_COMPRESS, saved_igzip_compress_);
    SetConfig(USE_ZLIB_COMPRESS, saved_zlib_compress_);
    SetConfig(USE_IAA_UNCOMPRESS, saved_iaa_uncompress_);
    SetConfig(USE_QAT_UNCOMPRESS, saved_qat_uncompress_);
    SetConfig(USE_IGZIP_UNCOMPRESS, saved_igzip_uncompress_);
    SetConfig(USE_ZLIB_UNCOMPRESS, saved_zlib_uncompress_);
    remove("file.gz");
  }

 private:
  uint32_t saved_iaa_compress_ = 0;
  uint32_t saved_qat_compress_ = 0;
  uint32_t saved_igzip_compress_ = 0;
  uint32_t saved_zlib_compress_ = 0;
  uint32_t saved_iaa_uncompress_ = 0;
  uint32_t saved_qat_uncompress_ = 0;
  uint32_t saved_igzip_uncompress_ = 0;
  uint32_t saved_zlib_uncompress_ = 0;
};

// gzeof has to answer for files the shim handed to zlib as well as the ones it
// decompressed itself. gz->read_past_end is only ever set by the accelerator
// read loop, so on the zlib path it stays false forever and gzeof must defer to
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

// gzwrite takes its own buffered route whenever any compress accelerator is
// enabled, so which one is selected does not matter to the close tests below --
// what matters is that one is, because with none of them the call delegates
// straight to zlib and the shim never buffers anything.
static ExecutionPath EnableSomeGzCompressPath(bool zlib_fallback = true) {
#if defined(USE_IGZIP)
  SetCompressPath(IGZIP, zlib_fallback, false, false);
  return IGZIP;
#elif defined(USE_QAT)
  SetCompressPath(QAT, zlib_fallback, false, false);
  return QAT;
#elif defined(USE_IAA)
  SetCompressPath(IAA, zlib_fallback, false, false);
  return IAA;
#else
  (void)zlib_fallback;
  SetCompressPath(ZLIB, false, false, false);
  return ZLIB;
#endif
}

// The read counterpart: gzread only takes its own buffered route while an
// uncompress accelerator is enabled, and the read helpers are only served by
// the shim for a file on that route.
static ExecutionPath EnableSomeGzUncompressPath() {
#if defined(USE_IGZIP)
  SetUncompressPath(IGZIP, /*zlib_fallback=*/true, false);
  return IGZIP;
#elif defined(USE_QAT)
  SetUncompressPath(QAT, /*zlib_fallback=*/true, false);
  return QAT;
#elif defined(USE_IAA)
  SetUncompressPath(IAA, /*zlib_fallback=*/true, false);
  return IAA;
#else
  SetUncompressPath(ZLIB, false, false);
  return ZLIB;
#endif
}

// gzwrite buffers up to data_buf_size before compressing, so the tail of a
// write is still in the shim's buffer when the application closes the file.
// gzclose flushes it; an application that closes through gzclose_w instead
// reached zlib directly, which knows nothing about that buffer, so the tail was
// dropped and the close still reported success.
TEST_F(GzipFileTest, GzcloseWFlushesBufferedData) {
  EnableSomeGzCompressPath();
  SetUncompressPath(ZLIB, false, false);

  // Over data_buf_size (256 KiB) so gzwrite compresses one full buffer and
  // leaves the remainder for the close to flush.
  const size_t input_length = 300 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e1);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(input_length)),
            static_cast<int>(input_length));
#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)
  // Not an ASSERT: the content check below is still worth running, but a zlib
  // path here means nothing was left buffered and the case proves nothing.
  EXPECT_NE(GetGzipFileExecutionPath(fp), ZLIB);
#endif
  ASSERT_EQ(gzclose_w(fp), Z_OK);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(input_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(input, uncompressed, input_length), 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

// zlib's gzclose_r and gzclose_w reject a file opened for the other direction
// without touching it. The shim has to reject it at the same point: the body it
// shares with gzclose flushes the buffer, closes the file and truncates it back
// to the size it recorded, none of which zlib would have done here.
TEST_F(GzipFileTest, GzcloseRejectsMismatchedMode) {
  EnableSomeGzCompressPath();
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 300 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e2);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(input_length)),
            static_cast<int>(input_length));

  // Reading close on a write file: rejected, and the file still writable.
  EXPECT_EQ(gzclose_r(fp), Z_STREAM_ERROR);
  ASSERT_EQ(gzclose_w(fp), Z_OK);

  // The rejected call must not have consumed the buffered data.
  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(input_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(input, uncompressed, input_length), 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

// The read direction of the same split. gzclose_r has to unregister the file
// and reach zlib's own close; a write close on it is rejected.
TEST_F(GzipFileTest, GzcloseRClosesReadFile) {
  SetCompressPath(ZLIB, false, false, false);
#if defined(USE_IAA)
  SetUncompressPath(IAA, true, false);
#elif defined(USE_QAT)
  SetUncompressPath(QAT, true, false);
#elif defined(USE_IGZIP)
  SetUncompressPath(IGZIP, true, false);
#else
  SetUncompressPath(ZLIB, false, false);
#endif

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e3);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(ZlibCompressGzipFile(input, input_length), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  std::vector<char> output(input_length, 0);
  ASSERT_EQ(gzread(fp, output.data(), static_cast<unsigned>(output.size())),
            static_cast<int>(input_length));
  EXPECT_EQ(memcmp(input, output.data(), input_length), 0);

  EXPECT_EQ(gzclose_w(fp), Z_STREAM_ERROR);
  EXPECT_EQ(gzclose_r(fp), Z_OK);

  remove(filename);
  DestroyBlock(input);
}

static size_t GzFileSize(const char* filename) {
  std::error_code ec;
  auto size = std::filesystem::file_size(filename, ec);
  return ec ? 0 : static_cast<size_t>(size);
}

// zlib folds append into its own write mode right after opening the file, which
// is why its gzclose_w accepts a file opened "ab" and why GzCloseModeMatches
// has to accept it too. Everything the shared close path does then runs on an
// appended file: the buffered tail is flushed, the file is truncated back to
// the size recorded before zlib's own close, and the entry is unregistered. A
// close that got any of that wrong on this mode would take the members already
// in the file with it.
TEST_F(GzipFileTest, GzcloseWClosesAppendedFile) {
  EnableSomeGzCompressPath();
  SetUncompressPath(ZLIB, false, false);

  const size_t first_length = 64 << 10;
  char* first = GenerateSeededCompressibleBlock(first_length, /*seed=*/0x11f2);
  ASSERT_NE(first, nullptr);
  // Over data_buf_size (256 KiB), so the append leaves a tail in the shim's
  // buffer for the close to flush instead of writing all of it from gzwrite.
  const size_t second_length = 300 << 10;
  char* second =
      GenerateSeededCompressibleBlock(second_length, /*seed=*/0x11f3);
  ASSERT_NE(second, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, first, static_cast<unsigned>(first_length)),
            static_cast<int>(first_length));
  ASSERT_EQ(gzclose(fp), Z_OK);
  const size_t size_after_first = GzFileSize(filename);
  ASSERT_GT(size_after_first, 0u);

  fp = gzopen(filename, "ab");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, second, static_cast<unsigned>(second_length)),
            static_cast<int>(second_length));
#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)
  // Not an ASSERT: the content check below is still worth running, but a zlib
  // path here means nothing was left buffered and the case proves nothing.
  EXPECT_NE(GetGzipFileExecutionPath(fp), ZLIB);
#endif
  EXPECT_EQ(gzclose_w(fp), Z_OK);
  EXPECT_GT(GzFileSize(filename), size_after_first);

  // The appended members decode after the first one, with nothing lost between.
  const size_t total_length = first_length + second_length;
  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(total_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, total_length);
  EXPECT_EQ(memcmp(first, uncompressed, first_length), 0);
  EXPECT_EQ(memcmp(second, uncompressed + first_length, second_length), 0);

  delete[] uncompressed;
  DestroyBlock(second);
  DestroyBlock(first);
}

// Writes the whole payload through the shim and returns the size of the file it
// produced. set_level >= 0 asks for the level through gzsetparams once the file
// is open, which is the request the mode string cannot express. 0 on any
// failure, so a caller asserting on a size catches it.
static size_t GzWriteFileAndGetSize(const char* filename, const char* mode,
                                    const char* input, size_t length,
                                    int set_level) {
  remove(filename);
  gzFile fp = gzopen(filename, mode);
  if (fp == nullptr) {
    return 0;
  }
  if (set_level >= 0 &&
      gzsetparams(fp, set_level, Z_DEFAULT_STRATEGY) != Z_OK) {
    gzclose(fp);
    return 0;
  }
  if (gzwrite(fp, input, static_cast<unsigned>(length)) !=
      static_cast<int>(length)) {
    gzclose(fp);
    return 0;
  }
  if (gzclose(fp) != Z_OK) {
    return 0;
  }
  return GzFileSize(filename);
}

// The level in a gzopen mode string used to be dropped: GetOpenFlags ignored
// the digit, so every file was compressed at the default level whatever the
// application asked for. Level 0 is the request with a visible contract -- it
// asks for stored, uncompressed blocks, which no backend can emit -- so the
// file has to be routed to zlib.
TEST_F(GzipFileTest, GzopenLevelZeroPinsToZlib) {
  // With use_zlib_compress off, only the pin can get this file to zlib: an
  // accelerator is enabled and zlib compression is not selected, so a write
  // that depends on the config instead of the pin writes nothing at all.
  EnableSomeGzCompressPath(/*zlib_fallback=*/false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 64 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e4);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb0");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(input_length)),
            static_cast<int>(input_length));
  EXPECT_EQ(GetGzipFileExecutionPath(fp), ZLIB);
  ASSERT_EQ(gzclose(fp), Z_OK);

  // Stored blocks are larger than what they store, which is what separates a
  // level that reached the compressor from one that was ignored.
  EXPECT_GT(GzFileSize(filename), input_length);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(input_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(input, uncompressed, input_length), 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

// A pin means zlib owns the file's output, and a write that never happened is
// not a reason to grant one. With every compress engine off, the first gzwrite
// is refused; if it records the zlib path anyway, the second one reads that
// leftover path as a pin and writes through zlib with use_zlib_compress still
// off, so two identical calls get two different answers. Both writes are
// refused here whatever the build, so this runs against zlib with no backend
// compiled in rather than skipping.
TEST_F(GzipFileTest, GzwriteRefusesEveryWriteWithNoEngineEnabled) {
  SetConfig(USE_IAA_COMPRESS, 0);
  SetConfig(USE_QAT_COMPRESS, 0);
  SetConfig(USE_IGZIP_COMPRESS, 0);
  SetConfig(USE_ZLIB_COMPRESS, 0);

  const char* filename = "file.gz";
  remove(filename);
  // Not "wb0": level 0 pins the file at gzopen, and a pinned write is meant to
  // reach zlib whatever the config says.
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);

  const char data[] = "no engine can take this";
  const unsigned len = static_cast<unsigned>(sizeof(data) - 1);
  EXPECT_EQ(gzwrite(fp, data, len), 0);
  EXPECT_EQ(GetGzipFileExecutionPath(fp), UNDEFINED);
  EXPECT_EQ(gzwrite(fp, data, len), 0);
  EXPECT_EQ(GetGzipFileExecutionPath(fp), UNDEFINED);

  EXPECT_EQ(gzclose(fp), Z_OK);
  EXPECT_EQ(GzFileSize(filename), 0u);
  remove(filename);
}

// The rest of the level range is only observable as a difference in ratio, and
// only on IGZIP: QAT and IAA take no compression level at all, so a level is
// recorded for them and cannot be honored.
TEST_F(GzipFileTest, GzopenAndGzsetparamsLevelReachTheCompressor) {
#if defined(USE_IGZIP)
  SetCompressPath(IGZIP, /*zlib_fallback=*/true, false, false);
#else
  GTEST_SKIP() << "no backend that takes a compression level is compiled in";
#endif
  SetUncompressPath(ZLIB, false, false);

  // Over data_buf_size, so the file is more than one member and the level has
  // to survive from one member to the next.
  const size_t input_length = 300 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e5);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  const size_t size_level_1 =
      GzWriteFileAndGetSize(filename, "wb1", input, input_length, -1);
  const size_t size_level_9 =
      GzWriteFileAndGetSize(filename, "wb9", input, input_length, -1);
  // Opened at 9, then asked for 1 before anything is written: the level in
  // effect is the one gzsetparams set, so this must match "wb1" exactly.
  const size_t size_set_to_1 =
      GzWriteFileAndGetSize(filename, "wb9", input, input_length, 1);

  ASSERT_GT(size_level_1, 0u);
  ASSERT_GT(size_level_9, 0u);
  ASSERT_GT(size_set_to_1, 0u);
  EXPECT_LT(size_level_9, size_level_1);
  EXPECT_EQ(size_set_to_1, size_level_1);

  remove(filename);
  DestroyBlock(input);
}

// gzsetparams down to level 0 mid-file is the gz analogue of deflateParams'
// level-0 pin: the rest of the file has to be routed to zlib, and the data
// already buffered has to reach the file as a member of its own first.
TEST_F(GzipFileTest, GzsetparamsLevelZeroRoutesRestOfFileToZlib) {
  // As above: use_zlib_compress off, so the second half reaches zlib only
  // through the pin.
  EnableSomeGzCompressPath(/*zlib_fallback=*/false);
  SetUncompressPath(ZLIB, false, false);

  const size_t half = 100 << 10;
  const size_t input_length = 2 * half;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e6);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);

  // Under data_buf_size, so this half is still in the shim's buffer when
  // gzsetparams is called.
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(half)),
            static_cast<int>(half));
  ASSERT_EQ(gzsetparams(fp, Z_NO_COMPRESSION, Z_DEFAULT_STRATEGY), Z_OK);
  EXPECT_EQ(GetGzipFileExecutionPath(fp), ZLIB);
  ASSERT_EQ(gzwrite(fp, input + half, static_cast<unsigned>(half)),
            static_cast<int>(half));
  ASSERT_EQ(gzclose(fp), Z_OK);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(input_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(input, uncompressed, input_length), 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

// zlib rejects gzsetparams on a file opened for reading. The shim has to
// forward before it acts, or it records a level for a file zlib refused.
TEST_F(GzipFileTest, GzsetparamsRejectsReadFile) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e7);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(ZlibCompressGzipFile(input, input_length), Z_OK);

  gzFile fp = gzopen("file.gz", "rb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzsetparams(fp, 1, Z_DEFAULT_STRATEGY), Z_STREAM_ERROR);
  EXPECT_EQ(gzclose(fp), Z_OK);

  DestroyBlock(input);
}

// Put gzwrite on the shim's own buffered route. What decides that is the config
// flag, not whether a backend was compiled in: with a compress flag set, the
// shim keeps the file and drives its own deflate stream, falling back to zlib's
// deflate for the compression itself when no accelerator is present. Without a
// flag, gzwrite hands the whole file to zlib and the shim's write buffer --
// with the seek gap it holds and the flush that empties it -- does not exist to
// be tested. So these tests run everywhere, including CI's no-accelerator
// build.
static void EnableShimOwnedGzWrites() {
  SetConfig(USE_IAA_COMPRESS, 0);
  SetConfig(USE_QAT_COMPRESS, 1);
  SetConfig(USE_IGZIP_COMPRESS, 0);
  SetConfig(USE_ZLIB_COMPRESS, 1);
}

// What every write-side call answers once a write has already failed. /dev/full
// makes that deterministic: every write to it fails with ENOSPC, so one file
// exercises all three entry points that flush.
//
// The answers are not all the same, and they are not all Z_ERRNO. zlib latches
// Z_ERRNO -- errno describes what happened -- and gzerror reports it, but
// gzflush and gzsetparams both refuse outright on a latched error and return
// Z_STREAM_ERROR instead of passing the code through. gzsetparams does so even
// when the level asked for is the one the file already has, because zlib checks
// the latch before it checks whether anything would change. Only the close
// hands the latched code back. Measured in bare zlib 1.3 with no shim loaded,
// one call at a time.
TEST_F(GzipFileTest, FlushFailureReportsZErrno) {
  if (access("/dev/full", W_OK) != 0) {
    GTEST_SKIP() << "/dev/full is not available";
  }
  EnableShimOwnedGzWrites();

  int fd = open("/dev/full", O_WRONLY);
  ASSERT_NE(fd, -1);
  gzFile fp = gzdopen(fd, "wb");
  ASSERT_NE(fp, nullptr);
  EXPECT_NE(GetGzipFileExecutionPath(fp), ZLIB);

  // Over data_buf_size, so the buffer fills and gzwrite has to flush it. The
  // write fails, so it reports having written nothing and the data stays
  // buffered for the calls below to trip over.
  const size_t input_length = 300 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11f4);
  ASSERT_NE(input, nullptr);
  EXPECT_EQ(gzwrite(fp, input, static_cast<unsigned>(input_length)), 0);

  // The failure itself is Z_ERRNO, and that is the code the file is holding.
  int err = Z_OK;
  gzerror(fp, &err);
  EXPECT_EQ(err, Z_ERRNO);

  // But these two refuse on the latch rather than reporting it.
  EXPECT_EQ(gzflush(fp, Z_SYNC_FLUSH), Z_STREAM_ERROR);
  EXPECT_EQ(gzsetparams(fp, 1, Z_DEFAULT_STRATEGY), Z_STREAM_ERROR);
  // Including when the level asked for is the one the file already has, which
  // would otherwise be a no-op: the latch is checked first.
  EXPECT_EQ(gzsetparams(fp, Z_DEFAULT_COMPRESSION, Z_DEFAULT_STRATEGY),
            Z_STREAM_ERROR);
  // The close is the one call that hands the latched code back.
  EXPECT_EQ(gzclose_w(fp), Z_ERRNO);

  DestroyBlock(input);
}

// The shim opens the file itself, before zlib ever validates the mode string,
// so a mode zlib refuses must not reach open(2): O_TRUNC would empty a file
// plain zlib leaves untouched, and O_CREAT would create one it never creates.
TEST_F(GzipFileTest, GzopenRejectsBadModeWithoutTouchingTheFile) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const char* filename = "file.gz";
  std::vector<char> input(4096, 'a');
  ASSERT_EQ(ZlibCompressGzipFile(input.data(), input.size()), Z_OK);
  const size_t size_before = GzFileSize(filename);
  ASSERT_GT(size_before, 0u);

  // '+' asks to read and write one file at once, which zlib refuses.
  EXPECT_EQ(gzopen(filename, "w+"), nullptr);
  EXPECT_EQ(GzFileSize(filename), size_before);

  // A mode string that names no direction is refused too.
  const char* missing = "file-mode-check.gz";
  remove(missing);
  EXPECT_EQ(gzopen(missing, "b"), nullptr);
  EXPECT_FALSE(std::filesystem::exists(missing));
  remove(missing);
}

// gzdopen does no mode validation of its own -- the fd is already open, so
// there is nothing to protect and it takes zlib's answer. What it must not do
// is register the NULL that answer can be: an entry keyed by NULL is what every
// gz* entry point finds when the application passes NULL, in place of the
// unregistered-file handling.
TEST_F(GzipFileTest, GzdopenRefusedModeRegistersNothing) {
  SetCompressPath(ZLIB, false, false, false);
  SetUncompressPath(ZLIB, false, false);

  const char* filename = "file.gz";
  remove(filename);
  int fd = open(filename, O_WRONLY | O_CREAT, 0666);
  ASSERT_GE(fd, 0);

  // Both of zlib's reasons for refusing a mode string: '+' asks for one file
  // read and written at once, and "b" names no direction at all.
  EXPECT_EQ(gzdopen(fd, "wb+"), nullptr);
  EXPECT_EQ(gzdopen(fd, "b"), nullptr);

  // A registered NULL would answer these from that entry's state instead.
  std::vector<uint8_t> buf(64, 0);
  EXPECT_EQ(gzwrite(nullptr, buf.data(), static_cast<unsigned>(buf.size())), 0);
  EXPECT_EQ(gzread(nullptr, buf.data(), static_cast<unsigned>(buf.size())), -1);
  EXPECT_EQ(gzflush(nullptr, Z_SYNC_FLUSH), Z_STREAM_ERROR);
  EXPECT_EQ(gzclose(nullptr), Z_STREAM_ERROR);

  // zlib leaves an fd whose mode it refused open, so this is still ours.
  EXPECT_EQ(close(fd), 0);
  remove(filename);
}

// Reads a file back through the shim and leaves it in place, unlike
// ZlibUncompressGzipFile -- the flush test reads a file that is still open for
// writing, and has to keep writing to it afterwards.
static std::string GzReadFileContents(const char* filename, size_t max_length) {
  std::string out;
  gzFile fp = gzopen(filename, "rb");
  if (fp == nullptr) {
    return out;
  }
  out.resize(max_length);
  size_t total = 0;
  int ret = 0;
  while (total < max_length &&
         (ret = gzread(fp, &out[0] + total,
                       static_cast<unsigned>(max_length - total))) > 0) {
    total += static_cast<size_t>(ret);
  }
  gzclose(fp);
  out.resize(ret < 0 ? 0 : total);
  return out;
}

// What gzflush owes the caller is that everything written so far is in the
// file. Forwarded to zlib it flushes a stream that has never seen this file's
// data, writing a gzip header into the middle of the members the shim wrote, so
// the flushed prefix does not read back and the file is left corrupt.
TEST_F(GzipFileTest, GzflushMakesEarlierWritesReadable) {
  EnableSomeGzCompressPath();
  SetUncompressPath(ZLIB, false, false);

  // Each half is under data_buf_size, so the first one is still in the shim's
  // buffer when gzflush is called and only the flush can put it in the file.
  const size_t half = 100 << 10;
  const size_t input_length = 2 * half;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e8);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(half)),
            static_cast<int>(half));
  ASSERT_EQ(gzflush(fp, Z_SYNC_FLUSH), Z_OK);

  // A second reader sees the flushed prefix, and nothing but it.
  std::string flushed = GzReadFileContents(filename, input_length);
  ASSERT_EQ(flushed.size(), half);
  EXPECT_EQ(memcmp(flushed.data(), input, half), 0);

  // Z_NO_FLUSH is in range for zlib, and has nothing left to write here.
  EXPECT_EQ(gzflush(fp, Z_NO_FLUSH), Z_OK);

  ASSERT_EQ(gzwrite(fp, input + half, static_cast<unsigned>(half)),
            static_cast<int>(half));
  ASSERT_EQ(gzclose(fp), Z_OK);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(input_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(input, uncompressed, input_length), 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

// The checks zlib makes before it flushes anything: a write-mode file and a
// flush value in range.
TEST_F(GzipFileTest, GzflushRejectsBadArguments) {
  EnableSomeGzCompressPath();
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11e9);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(input_length)),
            static_cast<int>(input_length));
  EXPECT_EQ(gzflush(fp, Z_FINISH + 1), Z_STREAM_ERROR);
  EXPECT_EQ(gzflush(fp, -1), Z_STREAM_ERROR);
  // A rejected flush must not have written the buffer out either, so the file
  // still round-trips through the close.
  ASSERT_EQ(gzclose(fp), Z_OK);

  gzFile rp = gzopen(filename, "rb");
  ASSERT_NE(rp, nullptr);
  EXPECT_EQ(gzflush(rp, Z_SYNC_FLUSH), Z_STREAM_ERROR);
  EXPECT_EQ(gzclose(rp), Z_OK);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(input_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(input, uncompressed, input_length), 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

// gzputs, gzputc, gzfwrite and gzprintf are write-through paths in zlib, so on
// a file the shim owns they have to reach the shim's own gzwrite. Forwarded,
// each writes through zlib's stream instead and the file decompresses with
// those bytes in a second member, out of order with the rest.
TEST_F(GzipFileTest, WriteHelpersInterleaveWithGzwrite) {
  EnableSomeGzCompressPath();
  SetUncompressPath(ZLIB, false, false);

  const size_t chunk = 64 << 10;
  char* input = GenerateSeededCompressibleBlock(2 * chunk, /*seed=*/0x11ea);
  ASSERT_NE(input, nullptr);

  const char* text = "helper writes go through the shim";
  const char* items = "0123456789abcdefghijklmnopqrstuv";
  const z_size_t item_size = 4;
  const z_size_t item_count = 8;

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);

  std::string expected;
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(chunk)),
            static_cast<int>(chunk));
  expected.append(input, chunk);

  EXPECT_EQ(gzputs(fp, text), static_cast<int>(strlen(text)));
  expected.append(text);

  EXPECT_EQ(gzputc(fp, 'X'), 'X');
  expected.push_back('X');

  EXPECT_EQ(gzfwrite(items, item_size, item_count, fp), item_count);
  expected.append(items, item_size * item_count);

  char formatted[64];
  int formatted_length =
      snprintf(formatted, sizeof(formatted), " n=%d s=%s ", 42, "printf");
  ASSERT_GT(formatted_length, 0);
  EXPECT_EQ(gzprintf(fp, " n=%d s=%s ", 42, "printf"), formatted_length);
  expected.append(formatted, static_cast<size_t>(formatted_length));

  ASSERT_EQ(gzwrite(fp, input + chunk, static_cast<unsigned>(chunk)),
            static_cast<int>(chunk));
  expected.append(input + chunk, chunk);

#if defined(USE_IGZIP) || defined(USE_QAT) || defined(USE_IAA)
  // No helper may hand the file back to zlib: it would spend the rest of the
  // file unaccelerated for the sake of one formatted string.
  EXPECT_NE(GetGzipFileExecutionPath(fp), ZLIB);
#endif
  ASSERT_EQ(gzclose(fp), Z_OK);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(ZlibUncompressGzipFile(expected.size(), &uncompressed,
                                   &uncompressed_length),
            Z_OK);
  ASSERT_EQ(uncompressed_length, expected.size());
  EXPECT_EQ(memcmp(expected.data(), uncompressed, expected.size()), 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

// The read helpers all have to come out of the shim's own gzread. Each reads
// through zlib's gz stream otherwise, which on a file the shim is reading sits
// at an unrelated offset, so the bytes they return are from the middle of a
// compressed member. Interleaving them with gzread checks the position they all
// share.
TEST_F(GzipFileTest, ReadHelpersServeTheAcceleratedFile) {
  SetCompressPath(ZLIB, false, false, false);
  EnableSomeGzUncompressPath();

  // Two lines for gzgets, then a block for gzfread and gzread.
  const size_t block_length = 16 << 10;
  char* block = GenerateSeededCompressibleBlock(block_length, /*seed=*/0x11ec);
  ASSERT_NE(block, nullptr);
  std::string payload = "first line\nsecond line\n";
  payload.append(block, block_length);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  std::string got;

  // One byte, pushed back, and read again: the same byte both times.
  int first = gzgetc(fp);
  ASSERT_EQ(first, payload[0]);
  ASSERT_EQ(gzungetc(first, fp), first);
  ASSERT_EQ(gzgetc(fp), first);
  got.push_back(static_cast<char>(first));

  // The rest of the first line, then the second. The newline is kept.
  char line[64];
  ASSERT_EQ(gzgets(fp, line, sizeof(line)), line);
  EXPECT_STREQ(line, "irst line\n");
  got.append(line);
  ASSERT_EQ(gzgets(fp, line, sizeof(line)), line);
  EXPECT_STREQ(line, "second line\n");
  got.append(line);

  const z_size_t item_size = 4;
  const z_size_t item_count = 32;
  std::vector<char> items(item_size * item_count, 0);
  ASSERT_EQ(gzfread(items.data(), item_size, item_count, fp), item_count);
  got.append(items.data(), items.size());

  std::vector<char> rest(payload.size(), 0);
  int rest_length = gzread(fp, rest.data(), static_cast<unsigned>(rest.size()));
  ASSERT_GE(rest_length, 0);
  got.append(rest.data(), static_cast<size_t>(rest_length));

  ASSERT_EQ(got.size(), payload.size());
  EXPECT_EQ(got, payload);
  EXPECT_NE(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
  DestroyBlock(block);
}

// gzungetc hands back a byte that need not be one that was read, and may be
// called before any read, so it cannot be a step back in the shim's buffer. The
// bytes it stores are data like any other: a read returns them first, and the
// file is no longer at its end.
TEST_F(GzipFileTest, GzungetcPushesBytesBack) {
  SetCompressPath(ZLIB, false, false, false);
  EnableSomeGzUncompressPath();

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11ed);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(ZlibCompressGzipFile(input, input_length), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  // -1 is not a byte, so it is refused whatever the state of the file.
  EXPECT_EQ(gzungetc(-1, fp), -1);

  std::vector<char> output(input_length, 0);
  ASSERT_EQ(gzread(fp, output.data(), static_cast<unsigned>(output.size())),
            static_cast<int>(input_length));
  EXPECT_EQ(memcmp(input, output.data(), input_length), 0);

  // One more read to take the file past its end, so gzeof answers yes.
  char tail = 0;
  EXPECT_EQ(gzread(fp, &tail, 1), 0);
  ASSERT_NE(gzeof(fp), 0);

  // A byte pushed back at end of file is readable again, and the file is no
  // longer at its end.
  ASSERT_EQ(gzungetc('Q', fp), 'Q');
  EXPECT_EQ(gzeof(fp), 0);
  // A second push is accepted, and the two come back in reverse order.
  ASSERT_EQ(gzungetc('R', fp), 'R');
  EXPECT_EQ(gzgetc(fp), 'R');
  EXPECT_EQ(gzgetc(fp), 'Q');
  EXPECT_EQ(gzgetc(fp), -1);
  EXPECT_NE(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
  DestroyBlock(input);
}

// gzgets stops at a newline or at one byte short of the buffer, whichever comes
// first, and reports end of file as no string rather than an empty one.
TEST_F(GzipFileTest, GzgetsStopsAtNewlineAndAtBufferSize) {
  SetCompressPath(ZLIB, false, false, false);
  EnableSomeGzUncompressPath();

  const std::string payload = "abc\ndefghij\n";
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  char line[64];
  // Room for three bytes, and no newline among them, so the line is cut there.
  ASSERT_EQ(gzgets(fp, line, 4), line);
  EXPECT_STREQ(line, "abc");
  // The newline it stopped short of comes back on its own.
  ASSERT_EQ(gzgets(fp, line, sizeof(line)), line);
  EXPECT_STREQ(line, "\n");
  ASSERT_EQ(gzgets(fp, line, 5), line);
  EXPECT_STREQ(line, "defg");
  ASSERT_EQ(gzgets(fp, line, sizeof(line)), line);
  EXPECT_STREQ(line, "hij\n");
  EXPECT_EQ(gzgets(fp, line, sizeof(line)), nullptr);

  // Arguments zlib refuses outright.
  EXPECT_EQ(gzgets(fp, line, 0), nullptr);
  EXPECT_EQ(gzgets(fp, nullptr, sizeof(line)), nullptr);
  EXPECT_EQ(gzfread(line, 8, std::numeric_limits<z_size_t>::max() / 4, fp), 0u);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// zlib refuses a gzfwrite whose size * nitems does not fit, and writes nothing.
TEST_F(GzipFileTest, GzfwriteRejectsAnOverflowingRequest) {
  EnableSomeGzCompressPath();
  SetUncompressPath(ZLIB, false, false);

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11eb);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzfwrite(input, 8, std::numeric_limits<z_size_t>::max() / 4, fp),
            0u);
  // A zero-length request is refused the same way.
  EXPECT_EQ(gzfwrite(input, 0, 8, fp), 0u);
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(input_length)),
            static_cast<int>(input_length));
  ASSERT_EQ(gzclose(fp), Z_OK);

  // The refused calls contributed nothing, so the file is the payload alone.
  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(input_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(input, uncompressed, input_length), 0);

  delete[] uncompressed;
  DestroyBlock(input);
}

// zlib guarantees a push of at least a buffer's worth of bytes immediately
// after the file is opened, with nothing read yet, so push-back cannot be
// bounded at one byte. A caller pushing back a prefix it has peeked at relies
// on this.
TEST_F(GzipFileTest, GzungetcAcceptsSeveralPushesBeforeAnyRead) {
  SetCompressPath(ZLIB, false, false, false);
  EnableSomeGzUncompressPath();

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11ef);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(ZlibCompressGzipFile(input, input_length), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  ASSERT_EQ(gzungetc('A', fp), 'A');
  ASSERT_EQ(gzungetc('B', fp), 'B');
  ASSERT_EQ(gzungetc('C', fp), 'C');
  EXPECT_EQ(gzeof(fp), 0);

  // One read spanning all three pushes and the start of the file: the pushed
  // bytes come first, most recent first, and the file's own data follows.
  char output[8];
  memset(output, 0, sizeof(output));
  ASSERT_EQ(gzread(fp, output, 5), 5);
  EXPECT_EQ(memcmp(output, "CBA", 3), 0);
  EXPECT_EQ(memcmp(output + 3, input, 2), 0);

  // The pushes displaced nothing: the rest of the file follows those two bytes.
  std::vector<char> rest(input_length - 2, 0);
  ASSERT_EQ(gzread(fp, rest.data(), static_cast<unsigned>(rest.size())),
            static_cast<int>(rest.size()));
  EXPECT_EQ(memcmp(rest.data(), input + 2, rest.size()), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
  DestroyBlock(input);
}

// zlib sets its end-of-file indicator only for a read that tried to go past the
// end of the file and came up short, which zlib.h spells out: a request served
// by exactly the bytes that were left leaves gzeof false, and gzungetc clears
// the indicator because there is a byte to read again. "The file has no more
// data" is a different question, and answering that one instead reports end of
// file a call early. Every assertion here holds for zlib itself, so with no
// backend compiled in the case runs against zlib as its own oracle.
TEST_F(GzipFileTest, GzeofReportsEofOnlyAfterAShortRead) {
  SetCompressPath(ZLIB, false, false, false);
  EnableSomeGzUncompressPath();

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11f6);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(ZlibCompressGzipFile(input, input_length), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  // Exactly the length of the file, so the read is satisfied in full and has no
  // occasion to look past its end.
  std::vector<char> output(input_length, 0);
  ASSERT_EQ(gzread(fp, output.data(), static_cast<unsigned>(output.size())),
            static_cast<int>(input_length));
  EXPECT_EQ(memcmp(input, output.data(), input_length), 0);
  EXPECT_EQ(gzeof(fp), 0);

  // A pushed-back byte is data, and reading it is another request served in
  // full, so neither the push nor the read that consumes it ends the file.
  ASSERT_EQ(gzungetc('Q', fp), 'Q');
  EXPECT_EQ(gzeof(fp), 0);
  ASSERT_EQ(gzgetc(fp), 'Q');
  EXPECT_EQ(gzeof(fp), 0);

  // This is the read that comes up short, and the only one that ends the file.
  char tail = 0;
  EXPECT_EQ(gzread(fp, &tail, 1), 0);
  EXPECT_NE(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
  DestroyBlock(input);
}

// A length that does not fit in the int gzwrite and gzread return is refused
// before either touches the buffer -- the requests below describe two gigabytes
// the caller does not have. zlib answers 0 and -1 respectively, and gzfwrite
// and gzfread depend on that being the answer: they cut a larger request into
// chunks of this size, so a chunk beyond it ends their loop having transferred
// bytes it could not report.
TEST_F(GzipFileTest, GzwriteAndGzreadRejectALengthThatDoesNotFitInInt) {
  EnableSomeGzCompressPath();
  EnableSomeGzUncompressPath();

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11f1);
  ASSERT_NE(input, nullptr);
  const unsigned too_long =
      static_cast<unsigned>(std::numeric_limits<int>::max()) + 1u;

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(input_length)),
            static_cast<int>(input_length));
  // The refused call comes after the payload rather than before it: on a file
  // zlib serves, zlib latches the failure and refuses everything that follows,
  // so a good call after a refused one is not behavior to assert on either
  // side. In this order the content check below still covers the refusal
  // leaving nothing buffered behind it.
  EXPECT_EQ(gzwrite(fp, input, too_long), 0);
  ASSERT_EQ(gzclose(fp), Z_OK);

  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  std::vector<char> output(input_length, 0);
  ASSERT_EQ(gzread(fp, output.data(), static_cast<unsigned>(output.size())),
            static_cast<int>(input_length));
  EXPECT_EQ(memcmp(input, output.data(), input_length), 0);
  EXPECT_EQ(gzread(fp, output.data(), too_long), -1);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
  DestroyBlock(input);
}

// ---------------------------------------------------------------------------
// Position, seek and error state.
// ---------------------------------------------------------------------------

// EnableSomeGzUncompressPath falls back to SetUncompressPath(ZLIB) when no
// backend is compiled in, which clears every accelerator flag and so hands the
// file to zlib at open. That is not the state the bookkeeping below lives in.
//
// A deployed host keeps the flag set for a backend it has, and the shim reads
// the flag rather than the compile-time macro: the flag makes the shim own the
// descriptor, and if the backend is not there its own inflate stream does the
// decompressing. That combination is what the differential runs against plain
// zlib were captured under, and it is reachable with nothing compiled in, so it
// is what these tests ask for.
static void EnableShimOwnedGzReads() {
  SetConfig(USE_IAA_UNCOMPRESS, 0);
  SetConfig(USE_QAT_UNCOMPRESS, 1);
  SetConfig(USE_IGZIP_UNCOMPRESS, 0);
  SetConfig(USE_ZLIB_UNCOMPRESS, 1);
}

// The two mode-mismatch refusals. They live here rather than beside the other
// head-of-gzwrite checks because reaching the code that used to be wrong needs
// the file on the shim's route, and the helper that arranges that is the one
// directly above.
//
// A write to a read-mode file: zlib refuses it and returns 0, leaving nothing
// latched. The shim used to accept it, and the cost was not the return value --
// the buffer it copies into is the same one the read path serves out of, so the
// four bytes came back from the next gzread ahead of the file's own contents.
// That is what the third assertion covers, and it is the one that failed.
TEST_F(GzipFileTest, GzwriteOnAReadModeFileIsRefusedAndChangesNothing) {
  // Both halves are load-bearing, and EnableSomeGzCompressPath is not enough
  // for the first: on a build with no backend compiled in it clears every
  // compress flag, so gzwrite hands the call straight to zlib and never reaches
  // the branch that was wrong. The flag has to be set for the shim to keep the
  // write, and the read file has to be the shim's for gz->path to be anything
  // other than ZLIB. Checked by mutation -- with EnableSomeGzCompressPath here,
  // this test passes even with the guard deleted.
  EnableShimOwnedGzWrites();
  EnableShimOwnedGzReads();

  const char* payload = "hello world";
  const unsigned payload_length = 11;
  const char* filename = "file.gz";
  remove(filename);

  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, payload, payload_length),
            static_cast<int>(payload_length));
  ASSERT_EQ(gzclose(fp), Z_OK);

  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  EXPECT_EQ(gzwrite(fp, "XXXX", 4), 0);

  // Nothing latched. zlib tests the mode ahead of the length, so an oversized
  // write to a read-mode file is refused on the mode and never reaches the
  // length check that would have latched Z_DATA_ERROR.
  int err = Z_OK;
  gzerror(fp, &err);
  EXPECT_EQ(err, Z_OK);
  EXPECT_EQ(
      gzwrite(fp, "XXXX",
              static_cast<unsigned>(std::numeric_limits<int>::max()) + 1u),
      0);
  err = Z_OK;
  gzerror(fp, &err);
  EXPECT_EQ(err, Z_OK);

  // The refused writes left the read untouched: the file's own first byte is
  // still the first byte served, and the position is still zero.
  EXPECT_EQ(gztell(fp), static_cast<z_off_t>(0));
  char out[32] = {0};
  EXPECT_EQ(gzread(fp, out, sizeof(out) - 1), static_cast<int>(payload_length));
  EXPECT_STREQ(out, payload);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// The mirror, which gzread was missing for the same reason: the four other read
// entry points guard on the mode and it did not. zlib answers -1 and latches
// nothing, and the buffered write it was refused against must survive intact.
TEST_F(GzipFileTest, GzreadOnAWriteModeFileIsRefused) {
  EnableShimOwnedGzWrites();
  EnableShimOwnedGzReads();

  const char* payload = "hello world";
  const unsigned payload_length = 11;
  const char* filename = "file.gz";
  remove(filename);

  gzFile fp = gzopen(filename, "wb6");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, payload, payload_length),
            static_cast<int>(payload_length));

  // Two lengths, because the unguarded call went wrong in two different ways
  // depending on how much the write had buffered. Asking for less than is
  // buffered was served out of the write's own buffer -- the application got
  // its pending output back as if it were file content. Asking for more ran the
  // buffer out, reached the descriptor, and read(2) on a write-only fd failed
  // with EBADF; the Z_ERRNO that latched then made gzclose write nothing while
  // still returning Z_OK, which is why the round-trip below is part of the
  // test.
  char out[32] = {0};
  EXPECT_EQ(gzread(fp, out, 4), -1);
  EXPECT_STREQ(out, "");
  EXPECT_EQ(gzread(fp, out, sizeof(out) - 1), -1);
  // gzgetc reads through gzread, so it has to answer the same way.
  EXPECT_EQ(gzgetc(fp), -1);
  int err = Z_OK;
  gzerror(fp, &err);
  EXPECT_EQ(err, Z_OK);

  ASSERT_EQ(gzclose(fp), Z_OK);

  // The refusals did not disturb what was still buffered for the write.
  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  memset(out, 0, sizeof(out));
  EXPECT_EQ(gzread(fp, out, sizeof(out) - 1), static_cast<int>(payload_length));
  EXPECT_STREQ(out, payload);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// Sixteen bytes per record, each naming its own offset. A repeating payload
// would let a read from the wrong offset look correct -- which is how the
// original gzseek check passed while the shim was reading from byte 0.
static std::string PositionStampedPayload(size_t records) {
  std::string payload;
  payload.reserve(records * 16);
  for (size_t i = 0; i < records; i++) {
    // "[off" + 8 digits + "]" is 13 characters, and the record is zeroed first
    // so that the three bytes after it are ones this function chose rather than
    // whatever was on the stack -- the append below takes all 16. Keeping them
    // NUL also keeps snprintf's terminator in place, which is what lets a test
    // read 16 bytes and compare the result as a C string.
    char record[17] = {0};
    snprintf(record, sizeof(record), "[off%08zu]", i * 16);
    payload.append(record, 16);
  }
  return payload;
}

// gzopen64 is not a 64-bit variant of anything: in zlib it is the same function
// as gzopen, with the same signature and no offset argument. It exists only so
// the rename zlib.h performs under _FILE_OFFSET_BITS=64 has a symbol to land
// on.
//
// Leaving it unexported took the shim off the path entirely for any application
// built that way, and did so invisibly: zlib opened the file, the shim never
// learned of it, every other entry point delegated, and the file was correct
// end to end and merely unaccelerated. Correctness therefore cannot detect the
// bug. Read-ahead can. The shim pulls 512 KiB of compressed input per gzread
// where zlib's input buffer holds 16 KiB, so after one small gzread of a file
// larger than 16 KiB compressed, zlib cannot have consumed more than 16 KiB and
// the shim has consumed the lot. gzoffset reports which of the two happened.
TEST_F(GzipFileTest, Gzopen64RegistersTheFileWithTheShim) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(24000);
  const char* filename = "file.gz";
  remove(filename);

  gzFile fp = gzopen64(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, payload.data(), static_cast<unsigned>(payload.size())),
            static_cast<int>(payload.size()));
  ASSERT_EQ(gzclose(fp), Z_OK);

  // Between the two buffer sizes, so the comparison below can tell them apart.
  const auto compressed = std::filesystem::file_size(filename);
  ASSERT_GT(compressed, static_cast<uintmax_t>(16 << 10));
  ASSERT_LT(compressed, static_cast<uintmax_t>(512 << 10));

  fp = gzopen64(filename, "rb");
  ASSERT_NE(fp, nullptr);
  char buf[24];
  ASSERT_EQ(gzread(fp, buf, sizeof(buf)), static_cast<int>(sizeof(buf)));
  EXPECT_EQ(memcmp(buf, payload.data(), sizeof(buf)), 0);
  EXPECT_EQ(gztell(fp), static_cast<z_off_t>(sizeof(buf)));
  // Plain zlib cannot report more than the 16 KiB it is able to hold, so this
  // is only reachable with the file registered and the shim doing the reading.
  EXPECT_GT(gzoffset(fp), static_cast<z_off_t>(16 << 10));

  ASSERT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// ---------------------------------------------------------------------------
// Non-seekable descriptors.
//
// The shim runs its own two-byte header test on every file it might read. A
// descriptor that can be seeked gets those two bytes put back afterwards; a
// pipe cannot, so it keeps them and hands them to whoever reads next. That one
// difference is what this section is about, and it has consequences all the way
// out to gzseek. Every test below writes far less than a pipe's 64 KiB
// capacity, so nothing blocks on an unread pipe.

// Fills a pipe with plain bytes and returns the read end, write end closed.
static int PipeOfPlainBytes(const std::string& bytes) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  if (write(fds[1], bytes.data(), bytes.size()) !=
      static_cast<ssize_t>(bytes.size())) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  close(fds[1]);
  return fds[0];
}

// The same, but the bytes are a gzip member written through the shim.
static int PipeOfGzipBytes(const std::string& payload) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  gzFile w = gzdopen(fds[1], "wb6");
  if (w == nullptr) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  const bool ok =
      gzwrite(w, payload.data(), static_cast<unsigned>(payload.size())) ==
      static_cast<int>(payload.size());
  gzclose(w);
  if (!ok) {
    close(fds[0]);
    return -1;
  }
  return fds[0];
}

// The case that already worked and must keep working: the peek says gzip, its
// two bytes go back in at the front of io_buf, and the read loop reads on top
// of them. If those bytes were lost the header would be truncated and the read
// would fail outright.
TEST_F(GzipFileTest, GzipPipeIsReadThroughTheAccelerator) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  const int fd = PipeOfGzipBytes(payload);
  ASSERT_NE(fd, -1);

  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);
  std::string got(payload.size(), '\0');
  ASSERT_EQ(gzread(fp, &got[0], static_cast<unsigned>(got.size())),
            static_cast<int>(payload.size()));
  EXPECT_EQ(got, payload);
  // The peek said gzip, so this is not a transparent file.
  EXPECT_EQ(gzdirect(fp), 0);
  EXPECT_EQ(gztell(fp), static_cast<z_off_t>(payload.size()));
  EXPECT_EQ(gzclose(fp), Z_OK);
}

// The peek only sits at the front of io_buf for the first refill, so a stream
// bigger than io_buf is where it could be double-counted or lost at the
// boundary. A pipe holds 64 KiB, so this one needs a writer running alongside
// the reader -- which is also the shape a pipe is actually used in.
TEST_F(GzipFileTest, LargeGzipPipeSurvivesBufferRefills) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  // Poorly compressible, so the compressed stream is larger than the shim's
  // 512 KiB io_buf and the read loop has to refill at least once.
  const size_t size = 1 << 20;
  std::string payload;
  payload.reserve(size);
  uint32_t x = 0x12345678;
  for (size_t i = 0; i < size; i++) {
    x = x * 1664525u + 1013904223u;
    payload.push_back(static_cast<char>(x >> 24));
  }

  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  std::thread writer([&] {
    gzFile w = gzdopen(fds[1], "wb6");
    if (w == nullptr) {
      close(fds[1]);
      return;
    }
    gzwrite(w, payload.data(), static_cast<unsigned>(payload.size()));
    gzclose(w);
  });

  gzFile fp = gzdopen(fds[0], "rb");
  ASSERT_NE(fp, nullptr);
  std::string got;
  got.reserve(payload.size());
  std::vector<char> buf(64 << 10);
  int n;
  while ((n = gzread(fp, buf.data(), static_cast<unsigned>(buf.size()))) > 0) {
    got.append(buf.data(), static_cast<size_t>(n));
  }
  writer.join();

  EXPECT_EQ(n, 0);
  ASSERT_EQ(got.size(), payload.size());
  EXPECT_EQ(got, payload);
  EXPECT_EQ(gzdirect(fp), 0);
  EXPECT_EQ(gztell(fp), static_cast<z_off_t>(payload.size()));
  int err = 0;
  gzerror(fp, &err);
  EXPECT_EQ(err, Z_OK);
  EXPECT_EQ(gzclose(fp), Z_OK);
}

// The defect: with nothing known about the descriptor the shim assumed gzip and
// tried to inflate plain text, so gzread returned -1 on a pipe plain zlib reads
// without difficulty.
TEST_F(GzipFileTest, NonGzipPipeIsCopiedThrough) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string plain = "not gzip at all, just text on a pipe\n";
  const int fd = PipeOfPlainBytes(plain);
  ASSERT_NE(fd, -1);

  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);
  char buf[128];
  memset(buf, 0, sizeof(buf));
  ASSERT_EQ(gzread(fp, buf, sizeof(buf)), static_cast<int>(plain.size()));
  EXPECT_EQ(std::string(buf, plain.size()), plain);
  // Asked for more than there was, which is the only thing zlib's end-of-file
  // indicator is set by.
  EXPECT_EQ(gzeof(fp), 1);
  EXPECT_EQ(gzdirect(fp), 1);
  EXPECT_EQ(gztell(fp), static_cast<z_off_t>(plain.size()));
  int err = 0;
  gzerror(fp, &err);
  EXPECT_EQ(err, Z_OK);
  EXPECT_EQ(gzclose(fp), Z_OK);
}

// The second defect. Without an intercepted gzdirect, zlib is still in its LOOK
// state on a descriptor the shim is reading, so an application gzdirect makes
// zlib look right then -- pulling up to 8 KB out of the pipe into zlib's buffer
// and punching a hole in the front of the shim's input. The read after it is
// what proves nothing was taken.
TEST_F(GzipFileTest, GzdirectBeforeFirstReadDoesNotConsumeThePipe) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  const int fd = PipeOfGzipBytes(payload);
  ASSERT_NE(fd, -1);

  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzdirect(fp), 0);
  std::string got(payload.size(), '\0');
  ASSERT_EQ(gzread(fp, &got[0], static_cast<unsigned>(got.size())),
            static_cast<int>(payload.size()));
  EXPECT_EQ(got, payload);
  EXPECT_EQ(gzdirect(fp), 0);
  EXPECT_EQ(gzclose(fp), Z_OK);
}

// One byte is why the peek loops instead of trusting a single read: 0x1f alone
// is not a header, and zlib -- whose own load loops until its buffer is full --
// calls this transparent. An empty pipe is the same conclusion with no bytes.
TEST_F(GzipFileTest, ShortAndEmptyPipesAreTransparent) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const int fd = PipeOfPlainBytes(std::string(1, '\x1f'));
  ASSERT_NE(fd, -1);
  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);
  unsigned char buf[8] = {0};
  ASSERT_EQ(gzread(fp, buf, sizeof(buf)), 1);
  EXPECT_EQ(buf[0], 0x1f);
  EXPECT_EQ(gzdirect(fp), 1);
  EXPECT_EQ(gzeof(fp), 1);
  EXPECT_EQ(gzclose(fp), Z_OK);

  const int empty_fd = PipeOfPlainBytes("");
  ASSERT_NE(empty_fd, -1);
  gzFile empty = gzdopen(empty_fd, "rb");
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(gzread(empty, buf, sizeof(buf)), 0);
  EXPECT_EQ(gzdirect(empty), 1);
  EXPECT_EQ(gzeof(empty), 1);
  int err = 0;
  gzerror(empty, &err);
  EXPECT_EQ(err, Z_OK);
  EXPECT_EQ(gzclose(empty), Z_OK);
}

// gzdirect is gated on who owns the file, and a seekable file can be either, so
// the two answers come from two different places. The gzip file is the shim's
// and answers from the shim's own peek; the plain one was handed to zlib at
// open and answers from zlib's look. Both, because delegating the wrong way
// round would be invisible in only one of them.
TEST_F(GzipFileTest, GzdirectOnSeekableFilesAnswersFromWhoeverOwnsThem) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, payload.data(), static_cast<unsigned>(payload.size())),
            static_cast<int>(payload.size()));
  ASSERT_EQ(gzclose(fp), Z_OK);

  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzdirect(fp), 0);
  EXPECT_EQ(gzclose(fp), Z_OK);

  // The same file with no gzip header, which zlib reads by copying through.
  {
    std::ofstream plain(filename, std::ios::binary | std::ios::trunc);
    plain << "plain text in a file called .gz\n";
  }
  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzdirect(fp), 1);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// zlib's forward seek on a transparent file is not one behaviour but two, and
// which one you get depends on whether a read has happened. Before the first
// read zlib is still in LOOK, so the seek is lazy and succeeds, and the read
// that follows pays for it by reading and discarding -- which a pipe permits.
// After the first read zlib is in COPY, where gzseek64 lseeks the descriptor
// directly, and a pipe refuses that. Both halves are measured against plain
// zlib by the conformance suite's O12 check.
TEST_F(GzipFileTest, ForwardSeekOnATransparentPipeFollowsZlib) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string plain = "0123456789ABCDEF";
  int fd = PipeOfPlainBytes(plain);
  ASSERT_NE(fd, -1);
  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzseek(fp, 4, SEEK_SET), 4);
  char buf[8];
  memset(buf, 0, sizeof(buf));
  ASSERT_EQ(gzread(fp, buf, 4), 4);
  EXPECT_EQ(std::string(buf, 4), "4567");
  EXPECT_EQ(gztell(fp), 8);
  // Now a read has happened, so the same seek becomes an lseek on a pipe.
  EXPECT_EQ(gzseek(fp, 12, SEEK_SET), -1);
  EXPECT_EQ(gzclose(fp), Z_OK);
}

TEST_F(GzipFileTest, GztellCountsBytesOnBothSides) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  const char* filename = "file.gz";
  remove(filename);

  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gztell(fp), 0);
  ASSERT_EQ(gzwrite(fp, payload.data(), static_cast<unsigned>(payload.size())),
            static_cast<int>(payload.size()));
  // The write side is wrong without this: the shim buffers rather than handing
  // the bytes to zlib, so zlib's own position stays at 0 for the whole file.
  EXPECT_EQ(gztell(fp), static_cast<z_off_t>(payload.size()));
  EXPECT_EQ(gztell64(fp), static_cast<z_off64_t>(payload.size()));
  ASSERT_EQ(gzclose(fp), Z_OK);

  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gztell(fp), 0);
  char buf[48];
  ASSERT_EQ(gzread(fp, buf, 32), 32);
  EXPECT_EQ(gztell(fp), 32);
  EXPECT_EQ(gztell64(fp), 32);
  // A pushed-back byte is available again, so the position moves back with it.
  ASSERT_EQ(gzungetc(buf[31], fp), static_cast<unsigned char>(buf[31]));
  EXPECT_EQ(gztell(fp), 31);
  ASSERT_EQ(gzread(fp, buf, 1), 1);
  EXPECT_EQ(gztell(fp), 32);
  EXPECT_GT(gzoffset(fp), 0);
  EXPECT_EQ(gzoffset(fp), gzoffset64(fp));

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

TEST_F(GzipFileTest, GzseekReadsFromTheOffsetItReports) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(6000);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  char buf[17] = {0};

  // Forward, absolute. This is the reported failure: without the fix gzseek
  // returns 2560 and the read that follows comes back with byte 0 of the file.
  EXPECT_EQ(gzseek(fp, 2560, SEEK_SET), 2560);
  // gztell has to agree with what gzseek just promised, before any read has
  // made the skip real.
  EXPECT_EQ(gztell(fp), 2560);
  ASSERT_EQ(gzread(fp, buf, 16), 16);
  EXPECT_STREQ(buf, "[off00002560]");
  EXPECT_EQ(gztell(fp), 2576);

  // Backwards, which has to rewind and skip forward again.
  EXPECT_EQ(gzseek(fp, 1024, SEEK_SET), 1024);
  ASSERT_EQ(gzread(fp, buf, 16), 16);
  EXPECT_STREQ(buf, "[off00001024]");

  // Relative, from wherever that left us.
  EXPECT_EQ(gzseek(fp, 496, SEEK_CUR), 1536);
  ASSERT_EQ(gzread(fp, buf, 16), 16);
  EXPECT_STREQ(buf, "[off00001536]");

  // Two seeks with no read between them: the second replaces the first rather
  // than adding to it, and only the second is paid for.
  EXPECT_EQ(gzseek(fp, 3200, SEEK_SET), 3200);
  EXPECT_EQ(gzseek(fp, 4096, SEEK_SET), 4096);
  ASSERT_EQ(gzread(fp, buf, 16), 16);
  EXPECT_STREQ(buf, "[off00004096]");

  // zlib's refusals: an unsupported whence, and a target before the start of
  // the file.
  EXPECT_EQ(gzseek(fp, 0, SEEK_END), -1);
  EXPECT_EQ(gzseek(fp, -1, SEEK_SET), -1);

  // A seek past the end lands at the end, and the read that follows is short
  // rather than wrong.
  EXPECT_EQ(gzseek(fp, static_cast<z_off_t>(payload.size()) + 4096, SEEK_SET),
            static_cast<z_off_t>(payload.size()) + 4096);
  EXPECT_EQ(gzread(fp, buf, 16), 0);
  EXPECT_NE(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

TEST_F(GzipFileTest, GzrewindStartsTheFileOver) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(2000);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  // Read well past the shim's first refill so the rewind has real buffered
  // state to discard, and leave a pushed-back byte for it to drop too.
  std::vector<char> block(20000, 0);
  ASSERT_EQ(gzread(fp, block.data(), static_cast<unsigned>(block.size())),
            static_cast<int>(block.size()));
  ASSERT_EQ(gzungetc('X', fp), 'X');

  ASSERT_EQ(gzrewind(fp), 0);
  EXPECT_EQ(gztell(fp), 0);
  EXPECT_EQ(gzeof(fp), 0);

  char buf[17] = {0};
  ASSERT_EQ(gzread(fp, buf, 16), 16);
  EXPECT_STREQ(buf, "[off00000000]");

  // And the whole file still reads correctly from there, so the rewind put the
  // descriptor and the inflate stream back rather than just the counters.
  std::string got(buf, 16);
  std::vector<char> rest(payload.size(), 0);
  int rest_length = gzread(fp, rest.data(), static_cast<unsigned>(rest.size()));
  ASSERT_GT(rest_length, 0);
  got.append(rest.data(), static_cast<size_t>(rest_length));
  EXPECT_EQ(got, payload);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

TEST_F(GzipFileTest, GzseekOnAWriteFileFillsTheGapWithZeros) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);

  ASSERT_EQ(gzwrite(fp, "head", 4), 4);
  // zlib allows a forward seek while writing and writes zeros over the gap.
  EXPECT_EQ(gzseek(fp, 12, SEEK_CUR), 16);
  // The gap counts towards the position before anything has filled it, which is
  // the whole point of gzseek returning the offset it promises.
  EXPECT_EQ(gztell(fp), 16);
  ASSERT_EQ(gzwrite(fp, "tail", 4), 4);
  EXPECT_EQ(gztell(fp), 20);

  // Backwards is refused, there being nothing to go back to. Asserted after the
  // gap has been filled, not before: measured in bare zlib, a refused seek
  // abandons a skip that was still pending, so the sequence
  // "seek +12, refused seek, write" produces an 8-byte file in zlib as well.
  // Interesting, but it is zlib's behavior and not something to assert here.
  EXPECT_EQ(gzseek(fp, 0, SEEK_SET), -1);
  ASSERT_EQ(gzclose(fp), Z_OK);

  EnableShimOwnedGzReads();
  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  char buf[32] = {0};
  ASSERT_EQ(gzread(fp, buf, sizeof(buf)), 20);
  EXPECT_EQ(std::string(buf, 4), "head");
  EXPECT_EQ(std::string(buf + 4, 12), std::string(12, '\0'));
  EXPECT_EQ(std::string(buf + 16, 4), "tail");
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// A gap left by a seek that was never written past still has to reach the file:
// zlib fills it at close, so the file is 16 bytes long, not 4.
TEST_F(GzipFileTest, GzseekOnAWriteFileIsPaidForAtClose) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, "head", 4), 4);
  EXPECT_EQ(gzseek(fp, 12, SEEK_CUR), 16);
  ASSERT_EQ(gzclose(fp), Z_OK);

  EnableShimOwnedGzReads();
  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  char buf[32] = {0};
  EXPECT_EQ(gzread(fp, buf, sizeof(buf)), 16);
  EXPECT_EQ(std::string(buf, 4), "head");
  EXPECT_EQ(std::string(buf + 4, 12), std::string(12, '\0'));
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

TEST_F(GzipFileTest, GzerrorLatchesAndGzclearerrClearsIt) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const size_t input_length = 8192;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11f7);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(ZlibCompressGzipFile(input, input_length), Z_OK);

  const char* filename = "file.gz";

  // Corrupt the CRC32 in the gzip trailer: the 8th byte from the end, wherever
  // the member was written and whatever it contains.
  int fd = open(filename, O_RDWR);
  ASSERT_NE(fd, -1);
  off_t end = lseek(fd, 0, SEEK_END);
  ASSERT_GT(end, 8);
  unsigned char crc_byte = 0;
  ASSERT_EQ(pread(fd, &crc_byte, 1, end - 8), 1);
  crc_byte ^= 0xff;
  ASSERT_EQ(pwrite(fd, &crc_byte, 1, end - 8), 1);
  close(fd);

  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  int errnum = Z_OK;
  EXPECT_STREQ(gzerror(fp, &errnum), "");
  EXPECT_EQ(errnum, Z_OK);

  std::vector<char> output(input_length + 512, 0);
  int ret = 0;
  // Drain to the failure. How many bytes arrive first is deliberately not
  // asserted: it depends on which path decompresses the member, and this
  // test is about the error latch, not about byte counts.
  while ((ret = gzread(fp, output.data(),
                       static_cast<unsigned>(output.size()))) > 0) {
  }
  EXPECT_EQ(ret, -1);

  const char* message = gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_DATA_ERROR);
  ASSERT_NE(message, nullptr);
  // zlib formats the message as "<file name>: <what went wrong>".
  EXPECT_NE(std::string(message).find(filename), std::string::npos);

  // The latch is sticky: a second read is refused without touching the file.
  EXPECT_EQ(gzread(fp, output.data(), 16), -1);
  gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_DATA_ERROR);

  // And gzclearerr takes it back off, which is what makes the file readable
  // again rather than permanently dead.
  gzclearerr(fp);
  gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_OK);
  EXPECT_EQ(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
  DestroyBlock(input);
}

TEST_F(GzipFileTest, GzclearerrClearsEndOfFile) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(4);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  char buf[128] = {0};
  ASSERT_EQ(gzread(fp, buf, sizeof(buf)), static_cast<int>(payload.size()));
  ASSERT_NE(gzeof(fp), 0);
  gzclearerr(fp);
  EXPECT_EQ(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// gzbuffer takes a size only before any reading or writing, because that is
// when zlib would still be allocating. On a file the shim reads, zlib never
// allocates at all, so its answer would be "yes" forever; the shim has to
// replicate the refusal against its own state. The size itself is accepted and
// dropped -- the shim's buffers are fixed. A file zlib owns is the other case
// and delegates instead; see GzbufferOnAPlainFileIsHonouredByZlib.
TEST_F(GzipFileTest, GzbufferAcceptsOnlyBeforeAnyIo) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzbuffer(fp, 8192), 0);
  // A size zlib could not double without overflowing is refused.
  EXPECT_EQ(gzbuffer(fp, 0x80000000u), -1);
  // Below 8 is raised to 8 by zlib, not rejected.
  EXPECT_EQ(gzbuffer(fp, 1), 0);

  char buf[16];
  ASSERT_EQ(gzread(fp, buf, sizeof(buf)), static_cast<int>(sizeof(buf)));
  EXPECT_EQ(gzbuffer(fp, 16384), -1);
  EXPECT_EQ(gzclose(fp), Z_OK);

  remove(filename);
  fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzbuffer(fp, 8192), 0);
  ASSERT_EQ(gzwrite(fp, payload.data(), static_cast<unsigned>(payload.size())),
            static_cast<int>(payload.size()));
  EXPECT_EQ(gzbuffer(fp, 16384), -1);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// ---------------------------------------------------------------------------
// On a descriptor that cannot be seeked, the header test waits.
//
// Everywhere else the shim runs it at open, where the two bytes can be put
// back. On a pipe they cannot, and reading them early costs something zlib
// never charges: zlib performs no I/O at all inside gzopen or gzdopen and
// decides nothing about the file until the first read. These are the two ways
// that showed up when the peek was done at open for pipes as well.
// ---------------------------------------------------------------------------

// gzdopen on a pipe that is empty but still has a writer must return, because a
// single-threaded program is allowed to wrap the read end first and write to it
// afterwards. An open-time read blocks there and the program never gets control
// back.
//
// The open runs on a thread only so the test can survive its own failure: if
// the open does block, the write below unblocks it and the join succeeds, and
// the expectation reports it. Run inline, a regression here would hang the
// suite.
TEST_F(GzipFileTest, GzdopenOnAPipeWithNoDataYetDoesNotBlock) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  int fds[2];
  ASSERT_EQ(pipe(fds), 0);

  const std::string payload = PositionStampedPayload(64);
  gzFile fp = nullptr;
  std::promise<void> opened;
  std::future<void> opened_future = opened.get_future();
  std::thread opener([&] {
    fp = gzdopen(fds[0], "rb");
    opened.set_value();
  });

  const bool returned_before_any_data =
      opened_future.wait_for(std::chrono::seconds(5)) ==
      std::future_status::ready;

  // Written whether or not the open came back, so the thread is always
  // joinable.
  gzFile wp = gzdopen(fds[1], "wb6");
  ASSERT_NE(wp, nullptr);
  ASSERT_EQ(gzwrite(wp, payload.data(), static_cast<unsigned>(payload.size())),
            static_cast<int>(payload.size()));
  ASSERT_EQ(gzclose(wp), Z_OK);
  opener.join();

  EXPECT_TRUE(returned_before_any_data);
  ASSERT_NE(fp, nullptr);

  // And the header test still runs when it is needed, so the deferral costs the
  // file nothing.
  std::string got(payload.size(), '\0');
  ASSERT_EQ(gzread(fp, &got[0], static_cast<unsigned>(got.size())),
            static_cast<int>(payload.size()));
  EXPECT_EQ(got, payload);
  EXPECT_EQ(gzdirect(fp), 0);
  EXPECT_EQ(gzclose(fp), Z_OK);
}

// A non-blocking descriptor with nothing on it yet. An open-time read comes
// back EAGAIN, which the peek's error path latches as Z_ERRNO -- so the file is
// broken before the application has asked it for anything, and plain zlib
// reports no such error.
TEST_F(GzipFileTest, GzdopenOnANonBlockingPipeLatchesNoError) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  ASSERT_EQ(fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);

  gzFile fp = gzdopen(fds[0], "rb");
  ASSERT_NE(fp, nullptr);
  int err = Z_OK;
  gzerror(fp, &err);
  EXPECT_EQ(err, Z_OK);
  // Nothing has been read, so the gzbuffer opportunity is still open -- the
  // same fact from the other side. zlib refuses gzbuffer once it has allocated,
  // and an open-time read on this descriptor is exactly what would have made
  // it.
  EXPECT_EQ(gzbuffer(fp, 8192), 0);

  // The whole member goes in before the first read, so no read below meets an
  // empty pipe and O_NONBLOCK never comes into it.
  const std::string payload = PositionStampedPayload(64);
  gzFile wp = gzdopen(fds[1], "wb6");
  ASSERT_NE(wp, nullptr);
  ASSERT_EQ(gzwrite(wp, payload.data(), static_cast<unsigned>(payload.size())),
            static_cast<int>(payload.size()));
  ASSERT_EQ(gzclose(wp), Z_OK);

  std::string got(payload.size(), '\0');
  ASSERT_EQ(gzread(fp, &got[0], static_cast<unsigned>(got.size())),
            static_cast<int>(payload.size()));
  EXPECT_EQ(got, payload);
  gzerror(fp, &err);
  EXPECT_EQ(err, Z_OK);
  EXPECT_EQ(gzclose(fp), Z_OK);
}

// The other half of ForwardSeekOnATransparentPipeFollowsZlib, and the reason
// the lazy peek changes which flag gates that seek. gzdirect is the one call
// that makes zlib look without the application having read anything: after it
// zlib is in COPY, so gzseek lseeks the descriptor and a pipe refuses it. Keyed
// on "has any read happened" the shim would instead take the lazy path and
// return the offset, disagreeing with zlib on the same call sequence.
TEST_F(GzipFileTest, GzdirectThenSeekOnATransparentPipeIsRefused) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const int fd = PipeOfPlainBytes("0123456789ABCDEF");
  ASSERT_NE(fd, -1);
  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);

  EXPECT_EQ(gzdirect(fp), 1);
  EXPECT_EQ(gzseek(fp, 4, SEEK_SET), -1);
  // Refused, not damaged: the file still reads from where it was.
  char buf[8] = {0};
  ASSERT_EQ(gzread(fp, buf, 4), 4);
  EXPECT_EQ(std::string(buf, 4), "0123");
  EXPECT_EQ(gzclose(fp), Z_OK);
}

// ---------------------------------------------------------------------------
// Boundary cases the shim used to answer differently from zlib.
// ---------------------------------------------------------------------------

// zlib's gz_read and gz_write both return 0 for a zero-length request before
// they allocate anything and before they serve a pending seek. So a zero-length
// call is not the start of I/O: gzbuffer is still legal after it, and a
// promised seek is still outstanding. Measured in bare zlib with no shim
// loaded.
TEST_F(GzipFileTest, ZeroLengthIoIsNotTheStartOfIo) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  char buf[17] = {0};
  EXPECT_EQ(gzread(fp, buf, 0), 0);
  EXPECT_EQ(gzbuffer(fp, 8192), 0);

  // And it leaves a promised seek alone rather than paying for it.
  EXPECT_EQ(gzseek(fp, 32, SEEK_SET), 32);
  EXPECT_EQ(gzread(fp, buf, 0), 0);
  EXPECT_EQ(gztell(fp), 32);
  ASSERT_EQ(gzread(fp, buf, 16), 16);
  EXPECT_STREQ(buf, "[off00000032]");
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);

  fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzwrite(fp, "", 0), 0);
  EXPECT_EQ(gzbuffer(fp, 8192), 0);
  // The pending seek survives it here too, so the zeros land at the close and
  // not one call early.
  EXPECT_EQ(gzseek(fp, 16, SEEK_CUR), 16);
  EXPECT_EQ(gzwrite(fp, "", 0), 0);
  EXPECT_EQ(gztell(fp), 16);
  ASSERT_EQ(gzwrite(fp, payload.data(), static_cast<unsigned>(payload.size())),
            static_cast<int>(payload.size()));
  ASSERT_EQ(gzclose(fp), Z_OK);

  fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  std::string got(16 + payload.size(), 'x');
  ASSERT_EQ(gzread(fp, &got[0], static_cast<unsigned>(got.size())),
            static_cast<int>(got.size()));
  EXPECT_EQ(got, std::string(16, '\0') + payload);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// zlib's gzungetc calls gz_look, which allocates, so the gzbuffer opportunity
// is gone afterwards even though the caller has read nothing. Measured against
// libz 1.3 rather than read off the vendored copy: that gz_look call arrived in
// 1.2.12 and the copy predates it.
TEST_F(GzipFileTest, GzungetcEndsTheGzbufferOpportunity) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzbuffer(fp, 8192), 0);
  ASSERT_EQ(gzungetc('X', fp), 'X');
  EXPECT_EQ(gzbuffer(fp, 16384), -1);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// zlib's gzflush checks the flush value along with the mode and the error
// latch, all before it fills a pending seek. Filling first means a rejected
// call has already changed the file: the zeros are in it and the call still
// returns Z_STREAM_ERROR, so nothing tells the caller that happened.
//
// The gap is deliberately bigger than the shim's 256 KiB write buffer, because
// that is what makes the difference reach the file. A small gap is buffered and
// the file on disk looks the same either way.
TEST_F(GzipFileTest, GzflushRejectsBadFlushBeforeFillingASeek) {
  EnableShimOwnedGzWrites();
  SetUncompressPath(ZLIB, false, false);

  const z_off_t gap = 4 << 20;
  const char* filename = "file.gz";
  remove(filename);
  gzFile fp = gzopen(filename, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, "head", 4), 4);
  ASSERT_EQ(gzflush(fp, Z_SYNC_FLUSH), Z_OK);
  const auto size_before = std::filesystem::file_size(filename);

  ASSERT_EQ(gzseek(fp, gap, SEEK_CUR), gap + 4);
  EXPECT_EQ(gzflush(fp, Z_FINISH + 1), Z_STREAM_ERROR);
  EXPECT_EQ(std::filesystem::file_size(filename), size_before);
  EXPECT_EQ(gzflush(fp, -1), Z_STREAM_ERROR);
  EXPECT_EQ(std::filesystem::file_size(filename), size_before);

  // The seek is still owed, so a flush zlib accepts pays it.
  EXPECT_EQ(gzflush(fp, Z_SYNC_FLUSH), Z_OK);
  EXPECT_GT(std::filesystem::file_size(filename), size_before);
  EXPECT_EQ(gztell(fp), gap + 4);
  ASSERT_EQ(gzclose(fp), Z_OK);

  gzFile rp = gzopen(filename, "rb");
  ASSERT_NE(rp, nullptr);
  std::string got(static_cast<size_t>(gap) + 4, 'x');
  ASSERT_EQ(gzread(rp, &got[0], static_cast<unsigned>(got.size())),
            static_cast<int>(got.size()));
  EXPECT_EQ(got, "head" + std::string(static_cast<size_t>(gap), '\0'));
  EXPECT_EQ(gzclose(rp), Z_OK);
  remove(filename);
}

// /dev/full accepts an open and fails every write with ENOSPC, which is the one
// way to reach a write failure at close without a filesystem to fill up.
//
// gzclose has to report it. zlib's gzclose_w takes state->err as its return
// value, so a disk-full at close comes back as an error there; dropping the
// return of the zero-fill instead produces a short file and Z_OK, which is
// silent truncation.
//
// The gap is over the shim's 256 KiB write buffer for the same reason as in
// GzflushRejectsBadFlushBeforeFillingASeek: below that, the zero-fill buffers
// cleanly and it is the close's own flush that meets the failure.
TEST_F(GzipFileTest, GzcloseReportsAZeroFillThatCouldNotBeWritten) {
  EnableShimOwnedGzWrites();

  const int fd = open("/dev/full", O_WRONLY);
  if (fd == -1) {
    GTEST_SKIP() << "/dev/full is not available";
  }
  gzFile fp = gzdopen(fd, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzwrite(fp, "head", 4), 4);
  ASSERT_EQ(gzseek(fp, 4 << 20, SEEK_CUR), (4 << 20) + 4);
  EXPECT_NE(gzclose(fp), Z_OK);
}

// A file that started on an accelerator and then fell back to zlib for the rest
// of its life -- gzsetparams to level 0 is the supported way there. From that
// point zlib does the writing and zlib latches the failures, so the shim's own
// error field stays clean and gzerror would report Z_OK for a write that did
// not happen. Mirroring zlib's latch back is what makes the answer true again.
TEST_F(GzipFileTest, GzerrorReportsAFailedWriteZlibPerformed) {
  EnableSomeGzCompressPath(/*zlib_fallback=*/false);

  const int fd = open("/dev/full", O_WRONLY);
  if (fd == -1) {
    GTEST_SKIP() << "/dev/full is not available";
  }
  gzFile fp = gzdopen(fd, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(gzsetparams(fp, 0, Z_DEFAULT_STRATEGY), Z_OK);

  // Large enough that zlib's own buffer fills and it really writes, rather than
  // holding everything until the close.
  const std::string payload = PositionStampedPayload(1 << 16);
  const int written =
      gzwrite(fp, payload.data(), static_cast<unsigned>(payload.size()));
  EXPECT_LT(written, static_cast<int>(payload.size()));

  int err = Z_OK;
  const char* message = gzerror(fp, &err);
  EXPECT_NE(err, Z_OK);
  EXPECT_NE(message, nullptr);
  EXPECT_NE(gzclose(fp), Z_OK);
}

// The 32-bit position calls are not casts of the 64-bit ones: zlib narrows with
// a round trip and answers -1 for an offset that does not fit. That is
// unreachable where z_off_t is already 64 bits, so what this pins down is that
// the two families still agree -- the check was added around live code.
TEST_F(GzipFileTest, NarrowAndWidePositionCallsAgree) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(gzseek(fp, 32, SEEK_SET), gzseek64(fp, 32, SEEK_SET));
  char buf[17] = {0};
  ASSERT_EQ(gzread(fp, buf, 16), 16);
  EXPECT_EQ(gztell(fp), gztell64(fp));
  EXPECT_EQ(gztell(fp), 48);
  EXPECT_EQ(gzoffset(fp), gzoffset64(fp));
  EXPECT_GT(gzoffset(fp), 0);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// ---------------------------------------------------------------------------
// The open-time header test, and the descriptor ownership it settles.
// ---------------------------------------------------------------------------

// A file that is not a gzip member at all. zlib reads it straight through; the
// shim's job is to notice at open and stay out of the way, because it has no
// copy-through path of its own and used to return -1 for the whole file.
TEST_F(GzipFileTest, GzopenReadsAPlainFileThroughZlib) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const char* filename = "file.gz";
  remove(filename);
  const std::string plain = "not gzip at all, just text\n";
  FILE* raw = fopen(filename, "wb");
  ASSERT_NE(raw, nullptr);
  ASSERT_EQ(fwrite(plain.data(), 1, plain.size(), raw), plain.size());
  ASSERT_EQ(fclose(raw), 0);

  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  char buf[64] = {0};
  EXPECT_EQ(gzread(fp, buf, sizeof(buf)), static_cast<int>(plain.size()));
  EXPECT_EQ(std::string(buf, plain.size()), plain);
  EXPECT_NE(gzeof(fp), 0);
  int errnum = Z_OK;
  gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_OK);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// An empty .gz -- touched, truncated, or an interrupted write. zlib reads it as
// zero bytes at end of file; the shim used to return -1.
TEST_F(GzipFileTest, GzopenReadsAnEmptyFileAsEndOfFile) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const char* filename = "file.gz";
  remove(filename);
  FILE* raw = fopen(filename, "wb");
  ASSERT_NE(raw, nullptr);
  ASSERT_EQ(fclose(raw), 0);

  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  char buf[64] = {0};
  EXPECT_EQ(gzread(fp, buf, sizeof(buf)), 0);
  EXPECT_NE(gzeof(fp), 0);
  int errnum = Z_OK;
  gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_OK);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// Bytes after a complete member that cannot begin another one. zlib ignores
// them and reports a clean end of file; the shim used to return -1 and lose the
// entire payload with it.
TEST_F(GzipFileTest, GzreadIgnoresATrailerThatIsNotAMember) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(64);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  FILE* raw = fopen(filename, "ab");
  ASSERT_NE(raw, nullptr);
  const char junk[] = "THIS IS NOT A GZIP MEMBER";
  ASSERT_EQ(fwrite(junk, 1, sizeof(junk) - 1, raw), sizeof(junk) - 1);
  ASSERT_EQ(fclose(raw), 0);

  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  std::vector<char> output(payload.size() + 512, 0);
  EXPECT_EQ(gzread(fp, output.data(), static_cast<unsigned>(output.size())),
            static_cast<int>(payload.size()));
  EXPECT_EQ(std::string(output.data(), payload.size()), payload);
  EXPECT_EQ(gzread(fp, output.data(), 16), 0);
  EXPECT_NE(gzeof(fp), 0);
  int errnum = Z_OK;
  gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_OK);
  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// The invariant the whole ownership split depends on, and the one thing here
// that fails silently rather than loudly if it is wrong.
//
// Once the shim has read part of a file, zlib cannot be handed the rest. zlib
// has read nothing and so is still expecting a gzip header, and the descriptor
// is somewhere in the middle of the compressed stream; it would try to parse
// deflate output as a header and fail, or worse. So a file the shim has started
// must never reach orig_gzread -- and the branch that would send it there is
// chosen per call, from the configuration, not per file.
//
// Turning every uncompress flag off mid-read is the config change that used to
// flip that branch. The file has to keep reading correctly through it: which
// engine decompresses may change, but not who reads the descriptor. Measured
// against the version of this that borrowed zlib's header look, where zlib was
// left holding up to 8 KB of stale input as well: reading such a file through
// zlib returned 51,456 bytes of duplicates and then Z_DATA_ERROR.
TEST_F(GzipFileTest, ConfigChangeMidReadDoesNotHandARewoundFileToZlib) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(6000);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);

  std::string got;
  std::vector<char> buf(4096, 0);
  ASSERT_EQ(gzread(fp, buf.data(), 4096), 4096);
  got.append(buf.data(), 4096);

  // Every accelerator off, part way through. Without the fix the next read
  // takes the orig_gzread branch, where zlib meets the middle of a deflate
  // stream and reads it as a header.
  SetUncompressPath(ZLIB, false, false);

  int ret = 0;
  while ((ret = gzread(fp, buf.data(), static_cast<unsigned>(buf.size()))) >
         0) {
    got.append(buf.data(), static_cast<size_t>(ret));
  }
  EXPECT_EQ(ret, 0);
  int errnum = Z_OK;
  gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_OK);
  EXPECT_EQ(got.size(), payload.size());
  EXPECT_EQ(got, payload);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// The same branch, reached with the configuration untouched. zlib parses a
// level digit out of the mode string even in read mode, so "rb0" yields level
// 0, which no backend can serve, and the file is pinned to zlib at open. It
// must never have been rewound in the first place -- and unlike the case above,
// nothing about the configuration is involved, so this one is reachable in
// production.
TEST_F(GzipFileTest, GzopenLevelZeroReadFileIsNeverRewound) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(6000);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  gzFile fp = gzopen(filename, "rb0");
  ASSERT_NE(fp, nullptr);

  std::string got;
  std::vector<char> buf(4096, 0);
  int ret = 0;
  while ((ret = gzread(fp, buf.data(), static_cast<unsigned>(buf.size()))) >
         0) {
    got.append(buf.data(), static_cast<size_t>(ret));
  }
  EXPECT_EQ(ret, 0);
  int errnum = Z_OK;
  gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_OK);
  EXPECT_EQ(got.size(), payload.size());
  EXPECT_EQ(got, payload);
  EXPECT_NE(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// The three tests below all go through gzdopen on a descriptor the test opened
// itself, for one reason: it is the only way to watch the file offset from
// outside. What zlib does or does not read is invisible through the gzFile API
// and is exactly what is being asserted, so lseek(fd, 0, SEEK_CUR) is the
// measurement.

// Nothing has been read at open, on a gzip file the shim is going to own. Two
// bytes went off the descriptor and went back, so the offset is where zlib's
// own gzopen would have left it, and an application gzdirect is answered out of
// the shim's own test with no I/O at all.
//
// The regression this is here for is delegating gzdirect for this file. zlib
// has not looked, so the delegated call would make it look -- allocating, and
// pulling its whole input buffer out of the descriptor from under the shim's
// read loop.
TEST_F(GzipFileTest, GzdirectOnAShimOwnedGzipFileMovesNothing) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(6000);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  int fd = open(filename, O_RDONLY);
  ASSERT_GE(fd, 0);
  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);

  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), static_cast<off_t>(0));
  EXPECT_EQ(gzdirect(fp), 0);
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), static_cast<off_t>(0));
  // Asked twice, because the shim's answer is cached and zlib's is not: a
  // second call must not be the one that looks either.
  EXPECT_EQ(gzdirect(fp), 0);
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), static_cast<off_t>(0));

  // And the file still reads, which is the assertion that catches the peek
  // being left in gz->peek after the seek back: the read path seeds io_buf from
  // it, so inflate would be handed the two header bytes twice.
  std::string got;
  std::vector<char> buf(4096, 0);
  int ret = 0;
  while ((ret = gzread(fp, buf.data(), static_cast<unsigned>(buf.size()))) >
         0) {
    got.append(buf.data(), static_cast<size_t>(ret));
  }
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(got, payload);

  // Again from the top. gzrewind puts the descriptor back at gz->start, which
  // is only the right place if the peek was accounted for, and re-reads through
  // the same seeding.
  ASSERT_EQ(gzrewind(fp), 0);
  got.clear();
  while ((ret = gzread(fp, buf.data(), static_cast<unsigned>(buf.size()))) >
         0) {
    got.append(buf.data(), static_cast<size_t>(ret));
  }
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(got, payload);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// A file that is not a gzip member goes to zlib at open, and this is what that
// buys. zlib is in the state it would be in with no shim loaded -- nothing
// allocated, nothing read -- so gzbuffer is not just accepted but applied, and
// the size the caller asked for is the size zlib reads with.
//
// Measured through the offset because that is the only visible difference. The
// version of this that borrowed zlib's header look had already made zlib
// allocate at 512 bytes by the time the application could speak, so a gzbuffer
// of any size changed nothing and the file was read 512 bytes at a time.
TEST_F(GzipFileTest, GzbufferOnAPlainFileIsHonouredByZlib) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  // Comfortably more than zlib's 8 KiB default, so "read it all" and "read a
  // bufferful" are different offsets.
  const std::string plain = PositionStampedPayload(40000);
  const char* filename = "file.gz";
  remove(filename);
  FILE* raw = fopen(filename, "wb");
  ASSERT_NE(raw, nullptr);
  ASSERT_EQ(fwrite(plain.data(), 1, plain.size(), raw), plain.size());
  ASSERT_EQ(fclose(raw), 0);

  int fd = open(filename, O_RDONLY);
  ASSERT_GE(fd, 0);
  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);

  // The two peek bytes went back, so zlib starts where it always would.
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), static_cast<off_t>(0));
  EXPECT_EQ(gzbuffer(fp, 1u << 20), 0);

  char first = '\0';
  ASSERT_EQ(gzread(fp, &first, 1), 1);
  EXPECT_EQ(first, plain[0]);
  // One byte asked for, the whole file read: zlib filled the buffer it was told
  // to use. At the old 512 this offset was 1,024.
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), static_cast<off_t>(plain.size()));
  // And zlib's own refusal after allocating, which is the half of the fidelity
  // the shim used to have to imitate.
  EXPECT_EQ(gzbuffer(fp, 1u << 20), -1);

  std::string got(1, first);
  std::vector<char> buf(4096, 0);
  int ret = 0;
  while ((ret = gzread(fp, buf.data(), static_cast<unsigned>(buf.size()))) >
         0) {
    got.append(buf.data(), static_cast<size_t>(ret));
  }
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(got, plain);
  EXPECT_NE(gzeof(fp), 0);
  int errnum = Z_OK;
  gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_OK);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// The other side of the same line: a gzip file the shim reads gets the shim's
// own answer, which is 0 before any I/O and -1 after, and no size is applied
// because the shim's buffers are fixed. What must not happen is zlib being left
// holding a buffer for a file it is not reading.
TEST_F(GzipFileTest, GzbufferOnAShimOwnedGzipFileLeavesZlibUnallocated) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string payload = PositionStampedPayload(6000);
  ASSERT_EQ(ZlibCompressGzipFile(payload.data(), payload.size()), Z_OK);

  const char* filename = "file.gz";
  int fd = open(filename, O_RDONLY);
  ASSERT_GE(fd, 0);
  gzFile fp = gzdopen(fd, "rb");
  ASSERT_NE(fp, nullptr);

  EXPECT_EQ(gzbuffer(fp, 1u << 20), 0);
  // Still nothing read on zlib's behalf, which is what "unallocated" looks like
  // from out here. A delegated gzbuffer would have been refused instead.
  EXPECT_EQ(lseek(fd, 0, SEEK_CUR), static_cast<off_t>(0));

  char buf[16] = {0};
  ASSERT_EQ(gzread(fp, buf, sizeof(buf)), static_cast<int>(sizeof(buf)));
  EXPECT_EQ(std::string(buf, sizeof(buf)), payload.substr(0, sizeof(buf)));
  EXPECT_EQ(gzbuffer(fp, 1u << 20), -1);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// A multi-member file, which is what the trailing-trailer rule has to keep
// working: two members concatenated read as one stream, and the magic test that
// stops at a non-member trailer must not stop at a real second member -- even
// when the two bytes of its header land either side of a buffer boundary.
TEST_F(GzipFileTest, GzreadStillJoinsConcatenatedMembers) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const std::string first = PositionStampedPayload(500);
  const std::string second = PositionStampedPayload(700);

  const char* filename = "file.gz";
  remove(filename);
  gzFile w = gzopen(filename, "wb");
  ASSERT_NE(w, nullptr);
  ASSERT_EQ(gzwrite(w, first.data(), static_cast<unsigned>(first.size())),
            static_cast<int>(first.size()));
  ASSERT_EQ(gzclose(w), Z_OK);
  w = gzopen(filename, "ab");
  ASSERT_NE(w, nullptr);
  ASSERT_EQ(gzwrite(w, second.data(), static_cast<unsigned>(second.size())),
            static_cast<int>(second.size()));
  ASSERT_EQ(gzclose(w), Z_OK);

  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  std::vector<char> output(first.size() + second.size() + 512, 0);
  std::string got;
  int ret = 0;
  while ((ret = gzread(fp, output.data(),
                       static_cast<unsigned>(output.size()))) > 0) {
    got.append(output.data(), static_cast<size_t>(ret));
  }
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(got, first + second);
  EXPECT_NE(gzeof(fp), 0);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
}

// A member that stops part way through. zlib returns the bytes it managed to
// inflate and latches Z_BUF_ERROR, which -- unlike a data error -- does not
// stop a later read.
TEST_F(GzipFileTest, GzreadLatchesBufErrorOnATruncatedMember) {
  EnableSomeGzCompressPath();
  EnableShimOwnedGzReads();

  const size_t input_length = 16 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x11fb);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(ZlibCompressGzipFile(input, input_length), Z_OK);

  const char* filename = "file.gz";
  int fd = open(filename, O_RDWR);
  ASSERT_NE(fd, -1);
  off_t end = lseek(fd, 0, SEEK_END);
  ASSERT_GT(end, 64);
  // Cut the member short, well inside the deflate body.
  ASSERT_EQ(ftruncate(fd, end / 2), 0);
  close(fd);

  gzFile fp = gzopen(filename, "rb");
  ASSERT_NE(fp, nullptr);
  std::vector<char> output(input_length + 512, 0);
  int total = 0;
  int ret = 0;
  while ((ret = gzread(fp, output.data(),
                       static_cast<unsigned>(output.size()))) > 0) {
    total += ret;
  }
  // Some of it came back, and the stream ending early is recorded rather than
  // reported as a clean end of file.
  EXPECT_GT(total, 0);
  EXPECT_EQ(ret, 0);
  int errnum = Z_OK;
  const char* message = gzerror(fp, &errnum);
  EXPECT_EQ(errnum, Z_BUF_ERROR);
  ASSERT_NE(message, nullptr);
  EXPECT_NE(std::string(message).find(filename), std::string::npos);

  EXPECT_EQ(gzclose(fp), Z_OK);
  remove(filename);
  DestroyBlock(input);
}

// zlib's gz_open returns NULL for a null path before it looks at the mode
// (gzlib.c), so the shim has to check it in the same place: it opens the file
// itself, and the mode-rejection branch ahead of that open logs the path.
// Streaming a null const char* into an ostream sets badbit rather than crashing
// on this library, and the log stream is std::cout unless a log file is
// configured, so one such call silences every later log in the process and
// takes the application's own stdout with it. A null mode is deliberately not
// checked, because zlib does not check it either.
TEST_F(GzipFileTest, GzopenRejectsANullPath) {
  // The log the bad mode reaches is LOG_INFO, so it has to be enabled for this
  // to be more than an assertion about the return value.
  const uint32_t saved_log_level = GetConfig(LOG_LEVEL);
  SetConfig(LOG_LEVEL, 2);

  EXPECT_EQ(gzopen(nullptr, "rb"), nullptr);
  EXPECT_EQ(gzopen(nullptr, "wb"), nullptr);
  // A mode naming no direction, which is the branch that logs the path.
  EXPECT_EQ(gzopen(nullptr, "q"), nullptr);
  EXPECT_EQ(gzopen64(nullptr, "rb"), nullptr);
  EXPECT_EQ(gzopen64(nullptr, "wb"), nullptr);
  EXPECT_EQ(gzopen64(nullptr, "q"), nullptr);

  // Nothing above wrote to the log stream, so it is still usable.
  EXPECT_FALSE(std::cout.bad());

  SetConfig(LOG_LEVEL, saved_log_level);
}

// A write(2) the tests below control, for the two returns a real file will not
// produce on demand: a short write and a write that accepts nothing. A pipe
// cannot stand in for either -- a blocking one transfers the whole count
// however small it is, and a non-blocking one answers EAGAIN instead of a
// partial count -- so write itself is replaced. A definition here interposes
// for libzlib-accel.so because the executable is searched ahead of the
// libraries it loads, and it needs explicit default visibility to be exported
// at all, since the whole build is compiled -fvisibility=hidden.
//
// This sees every write in the process, gtest's own output included, so
// anything but a registered descriptor is passed straight through. The
// pass-through is the raw syscall rather than a dlsym of the real write, to
// keep it off the loader's path.
namespace {
std::atomic<int> g_write_limit_fd{-1};
std::atomic<size_t> g_write_limit_chunk{0};
std::atomic<bool> g_write_limit_once{false};
std::atomic<int> g_write_limit_hits{0};
// Forces the registered descriptor's write to fail outright with this errno
// instead of the chunk/zero behavior above. 0 means unused.
std::atomic<int> g_write_limit_fail_errno{0};
}  // namespace

#pragma GCC visibility push(default)
extern "C" ssize_t write(int fd, const void* buf, size_t count) {
  if (fd >= 0 && fd == g_write_limit_fd.load()) {
    g_write_limit_hits.fetch_add(1);
    if (g_write_limit_once.load()) {
      g_write_limit_fd.store(-1);
    }
    const int fail_errno = g_write_limit_fail_errno.load();
    if (fail_errno != 0) {
      errno = fail_errno;
      return -1;
    }
    const size_t chunk = g_write_limit_chunk.load();
    if (chunk == 0) {
      // Accepts nothing and sets no errno of its own, which is the case the
      // caller has to fill in for itself.
      return 0;
    }
    if (chunk < count) {
      count = chunk;
    }
  }
  return syscall(SYS_write, fd, buf, count);
}
#pragma GCC visibility pop

// Registers a descriptor with the interposer above and unregisters it on the
// way out, including on the early return an ASSERT_* performs: a registration
// left behind would truncate every later write in the process.
class ScopedWriteLimit {
 public:
  ScopedWriteLimit(int fd, size_t chunk, bool once) {
    g_write_limit_chunk.store(chunk);
    g_write_limit_once.store(once);
    g_write_limit_hits.store(0);
    g_write_limit_fail_errno.store(0);
    g_write_limit_fd.store(fd);
  }
  ~ScopedWriteLimit() {
    g_write_limit_fd.store(-1);
    g_write_limit_fail_errno.store(0);
  }
  ScopedWriteLimit(const ScopedWriteLimit&) = delete;
  ScopedWriteLimit& operator=(const ScopedWriteLimit&) = delete;

  // How many writes the limit applied to, so a test can show it was not
  // vacuous.
  int hits() const { return g_write_limit_hits.load(); }

  // Makes the registered descriptor's write fail outright with e rather than
  // the chunk/zero behavior above.
  void FailWithErrno(int e) { g_write_limit_fail_errno.store(e); }
};

// CompressAndWrite has to send the rest of the buffer from where the last write
// stopped. Rewriting from the start of io_buf would re-emit the bytes already
// accepted and drop the tail, leaving a file that is still valid gzip and
// decompresses to something else.
TEST_F(GzipFileTest, ShortWritesStillProduceTheWholeFile) {
  EnableShimOwnedGzWrites();
  SetUncompressPath(ZLIB, false, false);

  const char* filename = "file.gz";
  remove(filename);
  const size_t input_length = 300 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x5be1);
  ASSERT_NE(input, nullptr);

  int hits = 0;
  {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    ASSERT_NE(fd, -1);
    // Small enough that every flush takes many writes.
    ScopedWriteLimit limit(fd, /*chunk=*/1024, /*once=*/false);
    gzFile fp = gzdopen(fd, "wb");
    ASSERT_NE(fp, nullptr);
    EXPECT_NE(GetGzipFileExecutionPath(fp), ZLIB);
    EXPECT_EQ(gzwrite(fp, input, static_cast<unsigned>(input_length)),
              static_cast<int>(input_length));
    EXPECT_EQ(gzclose_w(fp), Z_OK);
    hits = limit.hits();
  }
  // A full-sized write would have taken a handful; each of those became 1 KiB
  // pieces.
  EXPECT_GT(hits, 10);

  char* uncompressed = nullptr;
  size_t uncompressed_length = 0;
  ASSERT_EQ(
      ZlibUncompressGzipFile(input_length, &uncompressed, &uncompressed_length),
      Z_OK);
  EXPECT_EQ(uncompressed_length, input_length);
  EXPECT_EQ(memcmp(uncompressed, input, input_length), 0);

  DestroyBlock(uncompressed);
  DestroyBlock(input);
}

// A write that accepts nothing is not an error by itself, so it leaves errno
// alone -- whatever an unrelated syscall put there last, including a success.
// The failure is still reported as Z_ERRNO and latched with strerror(errno), so
// the shim has to supply an errno of its own or the file ends up holding a
// message that describes no failure.
TEST_F(GzipFileTest, GzwriteReportsAWriteThatAcceptedNothing) {
  EnableShimOwnedGzWrites();

  const char* filename = "file.gz";
  remove(filename);
  const size_t input_length = 300 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x2c73);
  ASSERT_NE(input, nullptr);

  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  ASSERT_NE(fd, -1);
  gzFile fp = gzdopen(fd, "wb");
  ASSERT_NE(fp, nullptr);
  EXPECT_NE(GetGzipFileExecutionPath(fp), ZLIB);

  int ret = 0;
  {
    // One shot only. zlib's own gz_comp treats a zero return as no progress and
    // retries it forever, and the close below reaches that loop.
    ScopedWriteLimit limit(fd, /*chunk=*/0, /*once=*/true);
    errno = 0;
    ret = gzwrite(fp, input, static_cast<unsigned>(input_length));
    EXPECT_EQ(limit.hits(), 1);
  }

  // Nothing reached the file, and the reason is one an application can read.
  EXPECT_EQ(ret, 0);
  int err = Z_OK;
  const char* message = gzerror(fp, &err);
  EXPECT_EQ(err, Z_ERRNO);
  ASSERT_NE(message, nullptr);
  EXPECT_NE(std::string(message).find(strerror(EIO)), std::string::npos);

  gzclose_w(fp);
  DestroyBlock(input);
  remove(filename);
}

// The other outcome GzwriteReportsAWriteThatAcceptedNothing above does not
// reach: a real write(2) failure, which already carries its own errno rather
// than needing one supplied. Unlike the zero-return case, nothing between
// write() and the check may touch errno first -- Log() runs there and is
// itself a write(), a risk this forces a real errno (ENOSPC) through to make
// concrete, though it does not exercise Log()'s own flush contending for
// errno: forcing that deterministically would need a stdout write to fail on
// cue after write(gz->fd, ...) already has, and every way tried to force it
// either missed glibc's internal stdio path (a write() interposer, since
// glibc's own flush does not go through the public symbol one replaces) or
// landed on an earlier, unrelated flush instead of this one (a broken pipe on
// stdout, since std::cout latches failure on the first write it loses and
// answers every flush after that from the latch, not a new syscall).
TEST_F(GzipFileTest, GzwriteReportsARealWriteFailure) {
  EnableShimOwnedGzWrites();

  const char* filename = "file.gz";
  remove(filename);
  const size_t input_length = 300 << 10;
  char* input = GenerateSeededCompressibleBlock(input_length, /*seed=*/0x9ee1);
  ASSERT_NE(input, nullptr);

  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  ASSERT_NE(fd, -1);
  gzFile fp = gzdopen(fd, "wb");
  ASSERT_NE(fp, nullptr);
  EXPECT_NE(GetGzipFileExecutionPath(fp), ZLIB);

  int ret = 0;
  {
    ScopedWriteLimit limit(fd, /*chunk=*/0, /*once=*/true);
    limit.FailWithErrno(ENOSPC);
    errno = 0;
    ret = gzwrite(fp, input, static_cast<unsigned>(input_length));
    EXPECT_EQ(limit.hits(), 1);
  }

  EXPECT_EQ(ret, 0);
  int err = Z_OK;
  const char* message = gzerror(fp, &err);
  EXPECT_EQ(err, Z_ERRNO);
  ASSERT_NE(message, nullptr);
  EXPECT_NE(std::string(message).find(strerror(ENOSPC)), std::string::npos);

  gzclose_w(fp);
  DestroyBlock(input);
  remove(filename);
}

// The third caller of FlushBufferedWrite(), and the one that did not latch a
// failure the way GzwriteReportsARealWriteFailure above shows gzwrite() does:
// a level change with data still buffered forces the same flush, and until
// now nothing recorded its failure on the file.
TEST_F(GzipFileTest, GzsetparamsLatchesAFailedFlush) {
  EnableShimOwnedGzWrites();

  const size_t half = 100 << 10;  // Under data_buf_size, so it stays buffered.
  char* input = GenerateSeededCompressibleBlock(half, /*seed=*/0x5e77);
  ASSERT_NE(input, nullptr);

  const char* filename = "file.gz";
  remove(filename);
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  ASSERT_NE(fd, -1);
  gzFile fp = gzdopen(fd, "wb");
  ASSERT_NE(fp, nullptr);
  EXPECT_NE(GetGzipFileExecutionPath(fp), ZLIB);

  ASSERT_EQ(gzwrite(fp, input, static_cast<unsigned>(half)),
            static_cast<int>(half));

  int ret = 0;
  {
    ScopedWriteLimit limit(fd, /*chunk=*/0, /*once=*/true);
    limit.FailWithErrno(ENOSPC);
    errno = 0;
    ret = gzsetparams(fp, Z_NO_COMPRESSION, Z_DEFAULT_STRATEGY);
    EXPECT_EQ(limit.hits(), 1);
  }

  EXPECT_EQ(ret, Z_ERRNO);
  int err = Z_OK;
  const char* message = gzerror(fp, &err);
  EXPECT_EQ(err, Z_ERRNO);
  ASSERT_NE(message, nullptr);
  EXPECT_NE(std::string(message).find(strerror(ENOSPC)), std::string::npos);

  // The latch has to reach the guard every write path shares: a caller that
  // ignored gzsetparams()'s own return should still find the file refusing
  // to write rather than silently accepting more into a file zlib considers
  // failed.
  EXPECT_EQ(gzwrite(fp, input, static_cast<unsigned>(half)), 0);

  gzclose_w(fp);
  DestroyBlock(input);
  remove(filename);
}
