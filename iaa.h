// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef USE_IAA

#include <cstdint>
#include <memory>
#include <vector>

#include "qpl/qpl.h"

#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

inline constexpr unsigned int PREPENDED_BLOCK_LENGTH = 5;
inline constexpr unsigned int MAX_BUFFER_SIZE = (2 << 20);

// IAA's decompressor has a fixed 4 kB history buffer, so it can only follow a
// stream whose match distances stay inside a 2^12-byte window.
inline constexpr int IAA_MAX_HISTORY_WINDOW_BITS = 12;

class IAAJob {
 public:
  IAAJob() : jobs_(3) {}

  qpl_job* GetJob(qpl_path_t execution_path) {
    if (!jobs_[execution_path]) {
      InitJob(execution_path);
    }
    return jobs_[execution_path].get();
  }

  void DestroyJob(qpl_path_t execution_path);

 private:
  struct QplJobDeleter {
    void operator()(qpl_job* job) const {
      if (job) {
        qpl_fini_job(job);
        delete[] reinterpret_cast<char*>(job);
      }
    }
  };

  using QplJobPtr = std::unique_ptr<qpl_job, QplJobDeleter>;

  void InitJob(qpl_path_t execution_path);

  QplJobPtr CreateQplJob(uint32_t size) {
    return QplJobPtr(reinterpret_cast<qpl_job*>(new char[size]));
  }

  std::vector<QplJobPtr> jobs_;
};

int CompressIAA(uint8_t* input, uint32_t* input_length, uint8_t* output,
                uint32_t* output_length, qpl_path_t execution_path,
                int window_bits, uint32_t max_compressed_size = 0,
                bool gzip_ext = false);

// window_too_large, when non-null, is set to true if the job was rejected
// because the stream references match distances beyond IAA's fixed 4 kB history
// buffer (QPL_STS_BAD_DIST_ERR). That is a property of whichever compressor
// produced the stream, not of the individual block, so a caller that sees it
// can stop offering the rest of that stream to IAA. It is never set to false;
// the caller owns initialisation.
VISIBLE_FOR_TESTING int UncompressIAA(uint8_t* input, uint32_t* input_length,
                                      uint8_t* output, uint32_t* output_length,
                                      qpl_path_t execution_path,
                                      int window_bits, bool* end_of_stream,
                                      bool detect_gzip_ext = false,
                                      bool* window_too_large = nullptr);

VISIBLE_FOR_TESTING bool SupportedOptionsIAA(int window_bits,
                                             uint32_t input_length,
                                             uint32_t output_length);

VISIBLE_FOR_TESTING bool IsIAADecompressible(uint8_t* input,
                                             uint32_t input_length,
                                             int window_bits);

// True if window_bits declares a maximum window IAA's history buffer can
// follow, whatever the format's own header says. inflateReset2() takes such a
// declaration from the caller, and it is a stronger statement than a remembered
// rejection: a stream that referenced further back than this would be refused
// by zlib too.
VISIBLE_FOR_TESTING bool DeclaresIAACompatibleWindow(int window_bits);

#endif  // USE_IAA
