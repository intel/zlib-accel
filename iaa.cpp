// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#ifdef USE_IAA

#include "iaa.h"

#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <utility>

#include "config/config.h"
#include "logging.h"

using namespace config;

void IAAJob::InitJob(qpl_path_t execution_path) {
  uint32_t size;
  qpl_status status = qpl_get_job_size(execution_path, &size);
  if (status != QPL_STS_OK) {
    return;
  }

  QplJobPtr job = nullptr;
  try {
    job = CreateQplJob(size);
  } catch (std::bad_alloc& e) {
    return;
  }
  status = qpl_init_job(execution_path, job.get());
  if (status != QPL_STS_OK) {
    return;
  }

  // Transfer ownership to the jobs_ vector
  jobs_[execution_path] = std::move(job);
}

void IAAJob::DestroyJob(qpl_path_t execution_path) {
  if (jobs_[execution_path]) {
    jobs_[execution_path].reset();
  }
}

static thread_local IAAJob job_;

uint32_t GetFormatFlag(int window_bits) {
  if (window_bits >= 8 && window_bits <= 15) {
    return QPL_FLAG_ZLIB_MODE;
  } else if (window_bits >= 24 && window_bits <= 31) {
    return QPL_FLAG_GZIP_MODE;
  }
  return 0;
}

int CompressIAA(uint8_t* input, uint32_t* input_length, uint8_t* output,
                uint32_t* output_length, qpl_path_t execution_path,
                int window_bits, uint32_t max_compressed_size, bool gzip_ext) {
  Log(LogLevel::LOG_INFO, "CompressIAA() Line ", __LINE__, " input_length ",
      *input_length, "\n");

  // State from previous job execution not ignored/reset correctly for zlib
  // format. Force job reinitialization.
  // TODO Remove when QPL has a fix
  if (window_bits == 15) {
    job_.DestroyJob(execution_path);
  }

  qpl_job* job = job_.GetJob(execution_path);
  if (!job) {
    Log(LogLevel::LOG_ERROR, "CompressIAA() Line ", __LINE__,
        " Error qpl_job is null\n");
    return 1;
  }

  job->next_in_ptr = input;
  job->available_in = *input_length;
  job->next_out_ptr = output;
  job->available_out = *output_length;
  job->level = qpl_default_level;
  job->op = qpl_op_compress;
  job->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST;
  job->flags |= QPL_FLAG_OMIT_VERIFY;
  job->flags |= QPL_FLAG_DYNAMIC_HUFFMAN;
  job->flags |= GetFormatFlag(window_bits);
  job->huffman_table = nullptr;
  job->dictionary = nullptr;

  uint32_t output_shift = 0;
  if (gzip_ext) {
    job->next_out_ptr += GZIP_EXT_XHDR_SIZE;
    if (job->available_out >= GZIP_EXT_XHDR_SIZE) {
      job->available_out -= GZIP_EXT_XHDR_SIZE;
    } else {
      return 1;
    }
    output_shift += GZIP_EXT_XHDR_SIZE;
  }

  // If prepending an empty block, leave space for it to be added
  // For zlib format, we don't need an empty block as a marker, as the zlib
  // header includes info about the window size
  bool prepend_empty_block = false;
  CompressedFormat format = GetCompressedFormat(window_bits);
  if (format != CompressedFormat::ZLIB &&
      configs[IAA_PREPEND_EMPTY_BLOCK] == 1 &&
      job->available_out >= PREPENDED_BLOCK_LENGTH) {
    job->next_out_ptr += PREPENDED_BLOCK_LENGTH;
    job->available_out -= PREPENDED_BLOCK_LENGTH;
    output_shift += PREPENDED_BLOCK_LENGTH;
    prepend_empty_block = true;
  }

  qpl_status status = qpl_execute_job(job);
  if (status != QPL_STS_OK) {
    Log(LogLevel::LOG_ERROR, "CompressIAA() Line ", __LINE__, " status ",
        status, "\n");
    return 1;
  }
  // In some cases, QPL compressed data size is larger than the upper bound
  // provided by zlib deflateBound.
  // TODO identify exact conditions and implement more permanent fix.
  if (max_compressed_size > 0 && job->total_out > max_compressed_size) {
    return 1;
  }

  *input_length = job->total_in;
  *output_length = job->total_out;

  Log(LogLevel::LOG_INFO, "CompressIAA() Line ", __LINE__, " compressed_size ",
      *output_length, "\n");

  if (output_shift > 0) {
    uint32_t pos = 0;

    // Move standard header to beginning of output
    uint32_t header_length = GetHeaderLength(format);
    for (uint32_t i = 0; i < header_length; i++) {
      output[i] = output[i + output_shift];
      pos++;
    }

    if (prepend_empty_block) {
      *output_length += PREPENDED_BLOCK_LENGTH;
    }

    // Add extended header
    if (gzip_ext) {
      // Set FLG.FEXTRA
      output[3] |= 0x4;

      output[pos++] = 12;  // XLEN
      output[pos++] = 0;
      output[pos++] = 'Q';  // SI1
      output[pos++] = 'Z';  // SI2
      output[pos++] = 8;    // LEN
      output[pos++] = 0;

      *(uint32_t*)(output + pos) = *input_length;
      pos += 4;
      *(uint32_t*)(output + pos) =
          *output_length - header_length - GetTrailerLength(format);
      pos += 4;

      *output_length += GZIP_EXT_XHDR_SIZE;
    }

    if (prepend_empty_block) {
      output[pos++] = 0;
      output[pos++] = 0;
      output[pos++] = 0;
      output[pos++] = 0xFF;
      output[pos] = 0xFF;
    }
  }

  return 0;
}

int UncompressIAA(uint8_t* input, uint32_t* input_length, uint8_t* output,
                  uint32_t* output_length, qpl_path_t execution_path,
                  int window_bits, bool* end_of_stream, bool detect_gzip_ext) {
  Log(LogLevel::LOG_INFO, "UncompressIAA() Line ", __LINE__, " input_length ",
      *input_length, "\n");

  bool gzip_ext = false;
  uint32_t gzip_ext_src_size = 0;
  uint32_t gzip_ext_dest_size = 0;
  if (detect_gzip_ext) {
    gzip_ext = DetectGzipExt(input, *input_length, &gzip_ext_src_size,
                             &gzip_ext_dest_size);
    // If gzip_ext is requested, fail if not found
    if (!gzip_ext) {
      return 1;
    }
  }

  qpl_job* job = job_.GetJob(execution_path);
  if (!job) {
    Log(LogLevel::LOG_ERROR, "UncompressIAA() Line ", __LINE__,
        " Error qpl_job is null\n");
    return 1;
  }

  job->next_in_ptr = input;
  job->available_in = *input_length;
  if (gzip_ext) {
    job->available_in = gzip_ext_dest_size + GZIP_EXT_HDRFTR_SIZE;
  }
  job->next_out_ptr = output;
  job->available_out = *output_length;
  job->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST;
  job->flags |= GetFormatFlag(window_bits);
  job->op = qpl_op_decompress;
  job->huffman_table = nullptr;
  job->dictionary = nullptr;

  qpl_status status = qpl_execute_job(job);
  if (status != QPL_STS_OK) {
    Log(LogLevel::LOG_ERROR, "UncompressIAA() Line ", __LINE__,
        " qpl_execute_job status ", status, "\n");
    return 1;
  }

  // TODO If reached EOS, consumed bytes is wrong. Requires IAA fix.
  //*input_length = job->total_in;
  *output_length = job->total_out;
  if (gzip_ext) {
    *input_length = gzip_ext_dest_size + GZIP_EXT_HDRFTR_SIZE;
  }
  *end_of_stream = true;
  Log(LogLevel::LOG_INFO, "UncompressIAA() Line ", __LINE__, " output size ",
      job->total_out, "\n");
  return 0;
}

bool SupportedOptionsIAA(int window_bits, uint32_t input_length,
                         uint32_t output_length) {
  if ((window_bits >= -15 && window_bits <= -8) ||
      (window_bits >= 8 && window_bits <= 15) ||
      (window_bits >= 24 && window_bits <= 31)) {
    if (input_length > MAX_BUFFER_SIZE || output_length > MAX_BUFFER_SIZE) {
      Log(LogLevel::LOG_INFO, "SupportedOptionsIAA() Line ", __LINE__,
          " input length ", input_length, " or output length ", output_length,
          " is more than 2MB\n");
      return false;
    }
    return true;
  }
  return false;
}

bool PrependedEmptyBlockPresent(uint8_t* input, uint32_t input_length,
                                CompressedFormat format) {
  uint32_t header_length = GetHeaderLength(format);
  if (header_length + PREPENDED_BLOCK_LENGTH > input_length) {
    return false;
  }

  if (input[header_length] == 0 && input[header_length + 1] == 0 &&
      input[header_length + 2] == 0 && input[header_length + 3] == 0xFF &&
      input[header_length + 4] == 0xFF) {
    Log(LogLevel::LOG_INFO, "PrependedEmptyBlockPresent() Line ", __LINE__,
        " Empty block detected\n");
    return true;
  }

  return false;
}

bool IsIAADecompressible(uint8_t* input, uint32_t input_length,
                         int window_bits) {
  CompressedFormat format = GetCompressedFormat(window_bits);
  if (format == CompressedFormat::ZLIB) {
    int window = GetWindowSizeFromZlibHeader(input, input_length);
    Log(LogLevel::LOG_INFO, "IsIAADecompressible() Line ", __LINE__, " window ",
        window, "\n");
    return window <= 12;
  } else {
    // if no empty block markers selected, we cannot tell for sure it's
    // IAA-decompression, but we assume it is.
    if (configs[IAA_PREPEND_EMPTY_BLOCK] == 0) {
      return true;
    } else if (configs[IAA_PREPEND_EMPTY_BLOCK] == 1 &&
               PrependedEmptyBlockPresent(input, input_length, format)) {
      return true;
    } else {
      return false;
    }
  }
}

#endif  // USE_IAA
