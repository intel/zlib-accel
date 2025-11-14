// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef USE_QAT

#include <qatzip.h>

#include <cstdint>
#include <memory>

#include "utils.h"

#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

inline constexpr unsigned int QAT_HW_BUFF_SZ = QZ_HW_BUFF_MAX_SZ;

class QATJob {
 public:
  QATJob() {}
  QzSession_T* GetQATSession(int window_bits, bool gzip_ext);
  void CloseQATSession(int window_bits, bool gzip_ext);

 private:
  struct QzSessionDeleter {
    void operator()(QzSession_T* session) const;
  };

  using QzSessionPtr = std::unique_ptr<QzSession_T, QzSessionDeleter>;

  void Init(QzSessionPtr& qzSession, CompressedFormat format,
            bool gzip_ext = false);

  QzSessionPtr qzSession_deflate_raw;
  QzSessionPtr qzSession_gzip;
  QzSessionPtr qzSession_gzip_ext;
  QzSessionPtr qzSession_zlib;
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
