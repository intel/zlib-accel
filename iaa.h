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

int UncompressIAA(uint8_t* input, uint32_t* input_length, uint8_t* output,
                  uint32_t* output_length, qpl_path_t execution_path,
                  int window_bits, bool* end_of_stream,
                  bool detect_gzip_ext = false);

VISIBLE_FOR_TESTING bool SupportedOptionsIAA(int window_bits,
                                             uint32_t input_length,
                                             uint32_t output_length);

VISIBLE_FOR_TESTING bool IsIAADecompressible(uint8_t* input,
                                             uint32_t input_length,
                                             int window_bits);

#endif  // USE_IAA
