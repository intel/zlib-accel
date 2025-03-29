// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef USE_QAT
#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

#include <qatzip.h>

#include <cstdio>

#include "utils.h"

#define QAT_HW_BUFF_SZ QZ_HW_BUFF_MAX_SZ

class QATJob {
 public:
  QATJob() {}
  ~QATJob() { Close(); }
  QzSession_T* GetQATSession(int window_bits, bool gzip_ext);
  void CloseQATSession(int window_bits, bool gzip_ext);

 private:
  void Init(QzSession_T** qzSession, CompressedFormat format,
            bool gzip_ext = false);
  void Close();
  void Close(QzSession_T* qzSession);

  QzSession_T* qzSession_deflate_raw = nullptr;
  QzSession_T* qzSession_gzip = nullptr;
  QzSession_T* qzSession_gzip_ext = nullptr;
  QzSession_T* qzSession_zlib = nullptr;
};

int CompressQAT(uint8_t* input, uint32_t* input_length, uint8_t* output,
                uint32_t* output_length, int window_bits,
                bool gzip_ext = false);

int UncompressQAT(uint8_t* input, uint32_t* input_length, uint8_t* output,
                  uint32_t* output_length, int window_bits, bool* end_of_stream,
                  bool detect_gzip_ext = false);

VISIBLE_FOR_TESTING bool SupportedOptionsQAT(int window_bits,
                                             uint32_t input_length);

#endif  // USE_QAT
