// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef USE_IAA
#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

#include <vector>

#include "qpl/qpl.h"

class IAAJob {
 public:
  IAAJob() : jobs_(3, nullptr) {}

  ~IAAJob() {
    for (qpl_job* job : jobs_) {
      if (job != nullptr) {
        qpl_fini_job(job);
        delete[] job;
      }
    }
  }

  qpl_job* GetJob(qpl_path_t execution_path) {
    if (jobs_[execution_path] == nullptr) {
      InitJob(execution_path);
    }
    return jobs_[execution_path];
  }

  void DestroyJob(qpl_path_t execution_path);

 private:
  void InitJob(qpl_path_t execution_path);

  std::vector<qpl_job*> jobs_;
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
