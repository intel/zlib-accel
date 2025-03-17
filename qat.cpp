// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "qat.h"

#include "config/config.h"
#include "logging.h"
#include "utils.h"
#ifdef USE_QAT
#include <iostream>

QzSession_T *QATJob::GetQATSession(int window_bits, bool gzip_ext) {
  CompressedFormat format = GetCompressedFormat(window_bits);
  switch (format) {
    case CompressedFormat::DEFLATE_RAW:
      if (qzSession_deflate_raw == nullptr) {
        Init(&qzSession_deflate_raw, format);
      }
      return qzSession_deflate_raw;
    case CompressedFormat::ZLIB:
      if (qzSession_zlib == nullptr) {
        Init(&qzSession_zlib,
             format);  // we do not have zlib format in public enum of qatzip
      }
      return qzSession_zlib;
    case CompressedFormat::GZIP:
      if (gzip_ext) {
        if (qzSession_gzip_ext == nullptr) {
          Init(&qzSession_gzip_ext, format, true);
        }
        return qzSession_gzip_ext;
      } else {
        if (qzSession_gzip == nullptr) {
          Init(&qzSession_gzip, format);
        }
        return qzSession_gzip;
      }
    case CompressedFormat::INVALID:
      return nullptr;
  }
  return nullptr;
}

void QATJob::CloseQATSession(int window_bits, bool gzip_ext) {
  CompressedFormat format = GetCompressedFormat(window_bits);
  switch (format) {
    case CompressedFormat::DEFLATE_RAW:
      Close(qzSession_deflate_raw);
      qzSession_deflate_raw = nullptr;
      break;
    case CompressedFormat::ZLIB:
      Close(qzSession_zlib);
      qzSession_zlib = nullptr;
      break;
    case CompressedFormat::GZIP:
      if (gzip_ext) {
        Close(qzSession_gzip_ext);
        qzSession_gzip_ext = nullptr;
      } else {
        Close(qzSession_gzip);
        qzSession_gzip = nullptr;
      }
      break;
    default:
      break;
  }
}

void QATJob::Init(QzSession_T **qzSession, CompressedFormat format,
                  bool gzip_ext) {
  try {
    *qzSession = new QzSession_T;
    memset(*qzSession, 0, sizeof(QzSession_T));
  } catch (std::bad_alloc &e) {
    *qzSession = nullptr;
    return;
  }
  // Initialize QAT hardware
  int status = qzInit(*qzSession, 0);
  if (status != QZ_OK && status != QZ_DUPLICATE) {
    Log(LogLevel::LOG_ERROR,
        "qzInit() failure  Line %d  session %p returned %d\n", __LINE__,
        *qzSession, status);
    delete *qzSession;
    *qzSession = nullptr;
    return;
  } else {
    Log(LogLevel::LOG_INFO,
        "qzInit() success  Line %d session %p returned %d\n", __LINE__,
        *qzSession, status);
  }

  QzSessionParamsDeflateExt_T deflateExt = {{}};
  deflateExt.deflate_params.common_params.comp_algorithm = QZ_DEFLATE;
  deflateExt.deflate_params.common_params.comp_lvl =
      config::qat_compression_level;
  deflateExt.deflate_params.common_params.direction = QZ_DIR_BOTH;
  deflateExt.deflate_params.common_params.hw_buff_sz = QAT_HW_BUFF_SZ;
  deflateExt.deflate_params.common_params.input_sz_thrshold =
      QZ_COMP_THRESHOLD_DEFAULT;
  deflateExt.deflate_params.common_params.is_sensitive_mode = 0;
  deflateExt.deflate_params.common_params.max_forks = 0;
  if (config::qat_periodical_polling == 1) {
    deflateExt.deflate_params.common_params.polling_mode =
        QZ_PERIODICAL_POLLING;
  } else {
    deflateExt.deflate_params.common_params.polling_mode = QZ_BUSY_POLLING;
  }
  deflateExt.deflate_params.common_params.req_cnt_thrshold = 32;
  deflateExt.deflate_params.common_params.strm_buff_sz =
      QZ_STRM_BUFF_SZ_DEFAULT;
  deflateExt.deflate_params.common_params.sw_backup = 0;
  deflateExt.deflate_params.common_params.wait_cnt_thrshold =
      QZ_WAIT_CNT_THRESHOLD_DEFAULT;
  deflateExt.deflate_params.huffman_hdr = QZ_HUFF_HDR_DEFAULT;
  deflateExt.stop_decompression_stream_end = 1;
  switch (format) {
    case CompressedFormat::DEFLATE_RAW:
      deflateExt.deflate_params.data_fmt = QZ_DEFLATE_RAW;
      break;
    case CompressedFormat::ZLIB:
      deflateExt.deflate_params.data_fmt = QZ_DEFLATE_RAW;
      deflateExt.zlib_format = 1;
      break;
    case CompressedFormat::GZIP:
      if (gzip_ext) {
        deflateExt.deflate_params.data_fmt = QZ_DEFLATE_GZIP_EXT;
      } else {
        deflateExt.deflate_params.data_fmt = QZ_DEFLATE_GZIP;
      }
      break;
    case CompressedFormat::INVALID:
      deflateExt.deflate_params.data_fmt = QZ_FMT_NUM;
      break;
  }
  status = qzSetupSessionDeflateExt(*qzSession, &deflateExt);
  if (status != QZ_OK) {
    Log(LogLevel::LOG_ERROR,
        "qzSetupSessionDeflateExt() Line %d session %p returned %d\n", __LINE__,
        *qzSession, status);
    Close();
    return;
  }
}

void QATJob::Close() {
  Close(qzSession_deflate_raw);
  qzSession_deflate_raw = nullptr;
  Close(qzSession_zlib);
  qzSession_zlib = nullptr;
  Close(qzSession_gzip);
  qzSession_gzip = nullptr;
}

void QATJob::Close(QzSession_T *qzSession) {
  if (!qzSession) {
    return;
  }

  int rc = qzTeardownSession(qzSession);
  if (rc != QZ_OK) {
    Log(LogLevel::LOG_ERROR,
        "qzTeardownSession() Line %d session %p returned %d\n", __LINE__,
        qzSession, rc);
  }

  // Attempt to close the session
  rc = qzClose(qzSession);
  if (rc != QZ_OK) {
    Log(LogLevel::LOG_ERROR, "qzClose() Line %d  session %p returned %d\n",
        __LINE__, qzSession, rc);
  }

  delete qzSession;
}

static thread_local QATJob qat_job_;

int CompressQAT(uint8_t *input, uint32_t *input_length, uint8_t *output,
                uint32_t *output_length, int window_bits, bool gzip_ext) {
  Log(LogLevel::LOG_INFO, "CompressQAT() Line %d input_length %d \n", __LINE__,
      *input_length);
  QzSession_T *qzSessObj = qat_job_.GetQATSession(window_bits, gzip_ext);
  if (qzSessObj == nullptr) {
    Log(LogLevel::LOG_ERROR, "CompressQAT() Line %d  Error qzSessObj null \n",
        __LINE__);
    return 1;
  }
  unsigned int src_buf_size = static_cast<unsigned int>(*input_length);
  unsigned int dst_buf_size = static_cast<unsigned int>(*output_length);
  int rc = qzCompress(qzSessObj, (unsigned char *)input, &src_buf_size,
                      (unsigned char *)output, &dst_buf_size, 1);
  if (rc != QZ_OK) {
    Log(LogLevel::LOG_ERROR,
        "CompressQAT() Line %d qzCompress returns status %d \n", __LINE__, rc);
    return rc;
  }
  *input_length = src_buf_size;
  *output_length = dst_buf_size;
  Log(LogLevel::LOG_INFO, "CompressQAT() Line %d compressed_size %d \n",
      __LINE__, *output_length);
  return 0;
}

int UncompressQAT(uint8_t *input, uint32_t *input_length, uint8_t *output,
                  uint32_t *output_length, int window_bits, bool *end_of_stream,
                  bool detect_gzip_ext) {
  Log(LogLevel::LOG_INFO, "UncompressQAT() Line %d input_length %d \n",
      __LINE__, *input_length);

  bool gzip_ext = false;
  uint32_t gzip_ext_src_size = 0;
  uint32_t gzip_ext_dest_size = 0;
  if (detect_gzip_ext) {
    gzip_ext = DetectGzipExt(input, *input_length, &gzip_ext_src_size,
                             &gzip_ext_dest_size);
  }

  QzSession_T *qzSessObj = qat_job_.GetQATSession(window_bits, gzip_ext);
  if (qzSessObj == nullptr) {
    Log(LogLevel::LOG_ERROR, "UncompressQAT() Line %d Error qzSessObj null \n",
        __LINE__);
    return 1;
  }

  unsigned int src_buf_size = static_cast<unsigned int>(*input_length);
  if (gzip_ext) {
    src_buf_size = gzip_ext_dest_size + GZIP_EXT_HDRFTR_SIZE;
  }
  unsigned int dst_buf_size = static_cast<unsigned int>(*output_length);
  int rc = qzDecompress(qzSessObj, (unsigned char *)input, &src_buf_size,
                        (unsigned char *)output, &dst_buf_size);
  if (rc != QZ_OK) {
    Log(LogLevel::LOG_ERROR,
        "UncompressQAT() Line %d qzDecompress status %d \n", __LINE__, rc);
    return 1;
  }
  *input_length = src_buf_size;
  *output_length = dst_buf_size;

  // if (qzSessObj->end_of_last_block == 0) {
  unsigned char qat_end_of_stream = 0;
  rc = qzGetDeflateEndOfStream(qzSessObj, &qat_end_of_stream);
  if (qat_end_of_stream == 0) {
    *end_of_stream = false;
    // Reset the QAT session
    // If QATzip used zlib and decompressed part of the stream correctly, it
    // will preserve zlib-related state in the session, which will impact future
    // decompressions
    // TODO ideally QATzip would provide a way to reset the relevant part of the
    // session, rather than closing it.
    qat_job_.CloseQATSession(window_bits, gzip_ext);
  } else {
    *end_of_stream = true;
  }

  Log(LogLevel::LOG_INFO,
      "UncompressQAT() Line %d output size %d  end_of_stream %d \n", __LINE__,
      dst_buf_size, *end_of_stream);
  return 0;
}

bool SupportedOptionsQAT(int window_bits, uint32_t input_length) {
  if ((window_bits >= -15 && window_bits <= -8) ||
      (window_bits >= 8 && window_bits <= 15) ||
      (window_bits >= 24 && window_bits <= 31)) {
    if (input_length < QZ_COMP_THRESHOLD_DEFAULT) {
      Log(LogLevel::LOG_INFO,
          "SupportedOptionsQAT() Line %d input length %d is less than "
          "QAT HW threshold\n",
          __LINE__, input_length);
      return false;
    }
    return true;
  }
  return false;
}

#endif  // USE_QAT
