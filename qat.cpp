// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#ifdef USE_QAT

#include "qat.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "config/config.h"
#include "logging.h"

using namespace config;

void QATJob::QzSessionDeleter::operator()(QzSession_T *qzSession) const {
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

QzSession_T *QATJob::GetQATSession(int window_bits, bool gzip_ext) {
  CompressedFormat format = GetCompressedFormat(window_bits);
  switch (format) {
    case CompressedFormat::DEFLATE_RAW:
      if (qzSession_deflate_raw == nullptr) {
        Init(qzSession_deflate_raw, format);
      }
      return qzSession_deflate_raw.get();
    case CompressedFormat::ZLIB:
      if (qzSession_zlib == nullptr) {
        Init(qzSession_zlib,
             format);  // we do not have zlib format in public enum of qatzip
      }
      return qzSession_zlib.get();
    case CompressedFormat::GZIP:
      if (gzip_ext) {
        if (qzSession_gzip_ext == nullptr) {
          Init(qzSession_gzip_ext, format, true);
        }
        return qzSession_gzip_ext.get();
      } else {
        if (qzSession_gzip == nullptr) {
          Init(qzSession_gzip, format);
        }
        return qzSession_gzip.get();
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
      qzSession_deflate_raw.reset();
      break;
    case CompressedFormat::ZLIB:
      qzSession_zlib.reset();
      break;
    case CompressedFormat::GZIP:
      if (gzip_ext) {
        qzSession_gzip_ext.reset();
      } else {
        qzSession_gzip.reset();
      }
      break;
    default:
      break;
  }
}

void QATJob::Init(QzSessionPtr &qzSession, CompressedFormat format,
                  bool gzip_ext) {
  QzSessionPtr session = nullptr;
  try {
    session = QzSessionPtr(new QzSession_T());
    memset(session.get(), 0, sizeof(QzSession_T));
  } catch (std::bad_alloc &e) {
    return;
  }

  // Initialize QAT hardware
  int status = qzInit(session.get(), 0);
  if (status != QZ_OK && status != QZ_DUPLICATE) {
    Log(LogLevel::LOG_ERROR, "qzInit() failure  Line ", __LINE__, "  session ",
        static_cast<void *>(session.get()), " returned ", status, "\n");
    return;
  } else {
    Log(LogLevel::LOG_INFO, "qzInit() success  Line ", __LINE__, " session ",
        static_cast<void *>(session.get()), " returned ", status, "\n");
  }

  QzSessionParamsDeflateExt_T deflateExt = {{}, 0, 0};
  deflateExt.deflate_params.common_params.comp_algorithm = QZ_DEFLATE;
  deflateExt.deflate_params.common_params.comp_lvl =
      configs[QAT_COMPRESSION_LEVEL];
  deflateExt.deflate_params.common_params.direction = QZ_DIR_BOTH;
  deflateExt.deflate_params.common_params.hw_buff_sz = QAT_HW_BUFF_SZ;
  deflateExt.deflate_params.common_params.input_sz_thrshold =
      QZ_COMP_THRESHOLD_DEFAULT;
  deflateExt.deflate_params.common_params.is_sensitive_mode = 0;
  deflateExt.deflate_params.common_params.max_forks = 0;
  if (configs[QAT_PERIODICAL_POLLING] == 1) {
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
  status = qzSetupSessionDeflateExt(session.get(), &deflateExt);
  if (status != QZ_OK) {
    Log(LogLevel::LOG_ERROR, "qzSetupSessionDeflateExt() Line ", __LINE__,
        " session ", static_cast<void *>(session.get()), " returned ", status,
        "\n");
    return;
  }

  // Transfer ownership to qzSession
  qzSession = std::move(session);
}

static thread_local QATJob qat_job_;

int CompressQAT(uint8_t *input, uint32_t *input_length, uint8_t *output,
                uint32_t *output_length, int window_bits, bool gzip_ext) {
  Log(LogLevel::LOG_INFO, "CompressQAT() Line ", __LINE__, " input_length ",
      *input_length, " \n");
  QzSession_T *qzSessObj = qat_job_.GetQATSession(window_bits, gzip_ext);
  if (qzSessObj == nullptr) {
    Log(LogLevel::LOG_ERROR, "CompressQAT() Line ", __LINE__,
        "  Error qzSessObj null \n");
    return 1;
  }
  unsigned int src_buf_size = static_cast<unsigned int>(*input_length);
  unsigned int dst_buf_size = static_cast<unsigned int>(*output_length);
  int rc = qzCompress(qzSessObj, (unsigned char *)input, &src_buf_size,
                      (unsigned char *)output, &dst_buf_size, 1);
  if (rc != QZ_OK) {
    Log(LogLevel::LOG_ERROR, "CompressQAT() Line ", __LINE__,
        " qzCompress returns status ", rc, " \n");
    return rc;
  }
  *input_length = src_buf_size;
  *output_length = dst_buf_size;
  Log(LogLevel::LOG_INFO, "CompressQAT() Line ", __LINE__, " compressed_size ",
      *output_length, " \n");
  return 0;
}

int UncompressQAT(uint8_t *input, uint32_t *input_length, uint8_t *output,
                  uint32_t *output_length, int window_bits, bool *end_of_stream,
                  bool detect_gzip_ext) {
  Log(LogLevel::LOG_INFO, "UncompressQAT() Line ", __LINE__, " input_length ",
      *input_length, " \n");

  bool gzip_ext = false;
  uint32_t gzip_ext_src_size = 0;
  uint32_t gzip_ext_dest_size = 0;
  if (detect_gzip_ext) {
    gzip_ext = DetectGzipExt(input, *input_length, &gzip_ext_src_size,
                             &gzip_ext_dest_size);
  }

  QzSession_T *qzSessObj = qat_job_.GetQATSession(window_bits, gzip_ext);
  if (qzSessObj == nullptr) {
    Log(LogLevel::LOG_ERROR, "UncompressQAT() Line ", __LINE__,
        " Error qzSessObj null \n");
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
    Log(LogLevel::LOG_ERROR, "UncompressQAT() Line ", __LINE__,
        " qzDecompress status ", rc, " \n");
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

  Log(LogLevel::LOG_INFO, "UncompressQAT() Line ", __LINE__, " output size ",
      dst_buf_size, "  end_of_stream ", *end_of_stream, " \n");
  return 0;
}

bool SupportedOptionsQAT(int window_bits, uint32_t input_length) {
  if ((window_bits >= -15 && window_bits <= -8) ||
      (window_bits >= 8 && window_bits <= 15) ||
      (window_bits >= 24 && window_bits <= 31)) {
    if (input_length < QZ_COMP_THRESHOLD_DEFAULT) {
      Log(LogLevel::LOG_INFO, "SupportedOptionsQAT() Line ", __LINE__,
          " input length ", input_length, " is less than QAT HW threshold\n");
      return false;
    }
    if (GetCompressedFormat(window_bits) != CompressedFormat::DEFLATE_RAW &&
        !configs[QAT_COMPRESSION_ALLOW_CHUNKING] &&
        input_length > QAT_HW_BUFF_SZ) {
      Log(LogLevel::LOG_INFO, "SupportedOptionsQAT() Line ", __LINE__,
          " input length ", input_length,
          " is greater than QAT HW buffer and chunking is not allowed\n");
      return false;
    }
    return true;
  }
  return false;
}

#endif  // USE_QAT
