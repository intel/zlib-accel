// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#ifdef USE_IGZIP

#include "igzip.h"

#include <stdlib.h>
#include <string.h>

#include "crc.h"
#include "logging.h"

static uint16_t ClampHistBits(int bits) {
  if (bits < 0) {
    return 0;
  }
  if (bits > ISAL_DEF_MAX_HIST_BITS) {
    return ISAL_DEF_MAX_HIST_BITS;
  }
  return (uint16_t)bits;
}

static constexpr uint32_t kIGZIPMinFinishOutputSize = 256;

static void ConfigureDeflateWindow(struct isal_zstream *isal_strm,
                                   int windowBits) {
  if (windowBits < 0) {
    isal_strm->gzip_flag = IGZIP_DEFLATE;
    isal_strm->hist_bits = ClampHistBits(-windowBits);
    return;
  }

  if (windowBits >= 24 && windowBits <= 31) {
    isal_strm->gzip_flag = IGZIP_GZIP;
    isal_strm->hist_bits = ClampHistBits(windowBits - 16);
    return;
  }

  isal_strm->gzip_flag = IGZIP_ZLIB;
  isal_strm->hist_bits = ClampHistBits(windowBits);
}

static void ConfigureInflateWindow(struct inflate_state *isal_strm_inflate,
                                   int windowBits) {
  if (windowBits < 0) {
    isal_strm_inflate->crc_flag = IGZIP_DEFLATE;
    isal_strm_inflate->hist_bits = ClampHistBits(-windowBits);
    return;
  }

  if ((windowBits >= 24 && windowBits <= 31) ||
      (windowBits >= 40 && windowBits <= 47)) {
    isal_strm_inflate->crc_flag = IGZIP_GZIP;
    isal_strm_inflate->hist_bits =
        ClampHistBits(windowBits > 31 ? windowBits - 32 : windowBits - 16);
    return;
  }

  isal_strm_inflate->crc_flag = IGZIP_ZLIB;
  isal_strm_inflate->hist_bits = ClampHistBits(windowBits);
}

bool IsIGZIPDeflateFinished(const struct isal_zstream *stream) {
  if (stream == nullptr) {
    return false;
  }
  const enum isal_zstate_state state = stream->internal_state.state;
  // ZSTATE_TMP_END is a temporary state and may require reentry to
  // flush remaining output; only ZSTATE_END is terminal.
  return state == ZSTATE_END;
}

bool SupportedOptionsIGZIPCompress(int flush, uint32_t output_length,
                                   bool stream_on_igzip_path) {
  if (flush != Z_FINISH) {
    Log(LogLevel::LOG_INFO, "SupportedOptionsIGZIPCompress() Line ", __LINE__,
        " flush ", flush, " is not Z_FINISH; IGZIP deflate path disabled\n");
    return false;
  }
  if (!stream_on_igzip_path && output_length < kIGZIPMinFinishOutputSize) {
    Log(LogLevel::LOG_INFO, "SupportedOptionsIGZIPCompress() Line ", __LINE__,
        " output length ", output_length,
        " is less than minimum finish buffer ", kIGZIPMinFinishOutputSize,
        "\n");
    return false;
  }
  return true;
}

bool SupportedOptionsIGZIPUncompress(int window_bits, uint32_t input_length,
                                     uint32_t output_length,
                                     bool stream_on_igzip_path) {
  (void)window_bits;
  (void)output_length;

  if (!stream_on_igzip_path && input_length == 0) {
    Log(LogLevel::LOG_INFO, "SupportedOptionsIGZIPUncompress() Line ", __LINE__,
        " fallback reason=no_input_on_new_stream input_length ", input_length,
        " output_length ", output_length, "\n");
    return false;
  }

  return true;
}

static bool IsIGZIPSyncFlush(int flush) {
  return flush == Z_SYNC_FLUSH || flush == Z_PARTIAL_FLUSH || flush == Z_BLOCK;
}

bool IGZIPShouldFallbackDeflate(bool stream_on_igzip_path, int flush,
                                uint32_t avail_in) {
  const bool is_streaming_flush =
      (flush == Z_SYNC_FLUSH || flush == Z_PARTIAL_FLUSH ||
       flush == Z_FULL_FLUSH || flush == Z_BLOCK);

  if (!stream_on_igzip_path && is_streaming_flush && avail_in > 0) {
    Log(LogLevel::LOG_INFO, "IGZIPShouldFallbackDeflate() Line ", __LINE__,
        " fallback reason=streaming_flush_with_input flush ", flush,
        " avail_in ", avail_in, "\n");
    return true;
  }

  if (!stream_on_igzip_path || avail_in != 0) {
    return false;
  }

  if (IsIGZIPSyncFlush(flush)) {
    Log(LogLevel::LOG_INFO, "IGZIPShouldFallbackDeflate() Line ", __LINE__,
        " fallback reason=empty_sync_flush_reentry flush ", flush, " avail_in ",
        avail_in, "\n");
    return true;
  }
  if (flush == Z_FINISH) {
    return false;
  }
  return false;
}

struct isal_zstream *InitCompressIGZIP(int level, int windowBits) {
  Log(LogLevel::LOG_INFO, "InitCompressIGZIP() Line ", __LINE__,
      " initializing deflate with level ", level, ", windowBits ", windowBits,
      "\n");

  struct isal_zstream *isal_strm =
      (struct isal_zstream *)malloc(sizeof(struct isal_zstream));
  if (!isal_strm) {
    Log(LogLevel::LOG_ERROR, "InitCompressIGZIP() Line ", __LINE__,
        " memory allocation for isal_zstream failed\n");
    return nullptr;
  }

  /* Setup ISA-L compression context */
  isal_deflate_init(isal_strm);

  isal_strm->end_of_stream = 0;
  isal_strm->flush = NO_FLUSH;

  // Map Zlib levels to ISA-L levels
  if (level >= 1 && level <= 2) {
    isal_strm->level = 1;
    isal_strm->level_buf = (uint8_t *)malloc(ISAL_DEF_LVL1_DEFAULT);
    isal_strm->level_buf_size = ISAL_DEF_LVL1_DEFAULT;
  } else if ((level >= 3 && level <= 6) || level == -1) {
    isal_strm->level = 2;
    isal_strm->level_buf = (uint8_t *)malloc(ISAL_DEF_LVL2_DEFAULT);
    isal_strm->level_buf_size = ISAL_DEF_LVL2_DEFAULT;
  } else if (level >= 7 && level <= 9) {
    isal_strm->level = 3;
    isal_strm->level_buf = (uint8_t *)malloc(ISAL_DEF_LVL3_DEFAULT);
    isal_strm->level_buf_size = ISAL_DEF_LVL3_DEFAULT;
  } else {
    Log(LogLevel::LOG_ERROR, "InitCompressIGZIP() Line ", __LINE__,
        " invalid compression level\n");
    free(isal_strm);
    return nullptr;
  }

  if (!isal_strm->level_buf) {
    free(isal_strm);
    Log(LogLevel::LOG_ERROR, "InitCompressIGZIP() Line ", __LINE__,
        " memory allocation for level_buf failed\n");
    return nullptr;
  }

  ConfigureDeflateWindow(isal_strm, windowBits);

  return isal_strm;
}

int CompressIGZIP(struct isal_zstream *isal_strm, int flush, uint8_t *input,
                  uint32_t *input_length, uint8_t *output,
                  uint32_t *output_length, unsigned long *total_in,
                  unsigned long *total_out) {
  int ret;

  (void)total_in;
  (void)total_out;
  if (!isal_strm) {
    Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
        " deflate isal_strm is NULL\n");
    return -1;
  }

  // set stream->avail_in, next_in, avail_out, next_out (from zstream)​
  isal_strm->next_out = output;
  const uint32_t original_avail_out = *output_length;
  isal_strm->avail_out = original_avail_out;
  isal_strm->next_in = input;
  const uint32_t original_avail_in = *input_length;
  isal_strm->avail_in = original_avail_in;
  isal_strm->total_out = *total_out;
  isal_strm->total_in = *total_in;

  // stream->flush mapping
  switch (flush) {
    case Z_NO_FLUSH:
      isal_strm->flush = NO_FLUSH;
      break;
    case Z_SYNC_FLUSH:
    case Z_PARTIAL_FLUSH:
    case Z_BLOCK:
      isal_strm->flush = SYNC_FLUSH;
      break;
    case Z_FULL_FLUSH:
      isal_strm->flush = FULL_FLUSH;
      break;
    case Z_FINISH:
      isal_strm->flush = FULL_FLUSH;
      isal_strm->end_of_stream = 1;
      break;
    default:
      Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
          " invalid flush value\n");
      return -1;
  }

  Log(LogLevel::LOG_INFO, "CompressIGZIP() Line ", __LINE__, " gzip_flag ",
      isal_strm->gzip_flag, ", hist_bits ", isal_strm->hist_bits, ", flush ",
      isal_strm->flush, ", level ", isal_strm->level, ", avail_in ",
      isal_strm->avail_in, ", avail_out ", (uint32_t)isal_strm->avail_out,
      ", total_out ", (uint32_t)isal_strm->total_out, ", total_in ",
      (uint32_t)isal_strm->total_in, "\n");

  int comp = isal_deflate(isal_strm);

  *output_length = original_avail_out - isal_strm->avail_out;
  *input_length = original_avail_in - isal_strm->avail_in;
  input = isal_strm->next_in;
  output = isal_strm->next_out;

  Log(LogLevel::LOG_INFO, "CompressIGZIP() Line ", __LINE__,
      " after isal_deflate: avail_in ", isal_strm->avail_in, ", avail_out ",
      (uint32_t)isal_strm->avail_out, ", bytes_consumed ", *input_length,
      ", bytes_produced ", *output_length, "\n");

  ret = (comp == COMP_OK) ? 0 : 1;

  if (ret == Z_OK) {
    Log(LogLevel::LOG_INFO, "CompressIGZIP() Line ", __LINE__,
        " deflate finished successfully Z_OK\n");
  } else if (ret == Z_STREAM_END) {
    Log(LogLevel::LOG_INFO, "CompressIGZIP() Line ", __LINE__,
        " deflate finished successfully Z_STREAM_END\n");
  } else {
    Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
        " deflate finished with error code ", ret, "\n");
    switch (comp) {
      case INVALID_FLUSH:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
            " invalid flush\n");
        break;
      case INVALID_PARAM:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
            " invalid parameter\n");
        break;
      case STATELESS_OVERFLOW:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
            " stateless overflow\n");
        break;
      case ISAL_INVALID_OPERATION:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
            " invalid operation\n");
        break;
      case ISAL_INVALID_STATE:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
            " invalid state\n");
        break;
      case ISAL_INVALID_LEVEL:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
            " invalid level\n");
        break;
      case ISAL_INVALID_LEVEL_BUF:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ", __LINE__,
            " invalid level buffer\n");
        break;
    }
  }

  return ret;
}

int EndCompressIGZIP(struct isal_zstream *isal_strm) {
  if (!isal_strm) {
    Log(LogLevel::LOG_ERROR, "EndCompressIGZIP() Line ", __LINE__,
        " isal_stream is NULL\n");
    return -1;
  }

  // Free allocated memory for level_buf and isal_strm
  if (isal_strm->level_buf) {
    free(isal_strm->level_buf);
  }
  free(isal_strm);

  Log(LogLevel::LOG_INFO, "EndCompressIGZIP() Line ", __LINE__,
      " deflate end\n");
  return Z_OK;
}

int deflateSetDictionary(z_streamp strm, unsigned char *dict_data,
                         unsigned int dict_len) {
  if (!strm || !strm->state || !dict_data || dict_len == 0)
    return Z_STREAM_ERROR;

  deflate_state *s = (deflate_state *)strm->state;

  if (!s || !s->isal_strm) return Z_STREAM_ERROR;

  return isal_deflate_set_dict(s->isal_strm, dict_data, dict_len);
}

unsigned long crc32(unsigned long crc, const unsigned char *buf,
                    unsigned int len) {
  return crc32_gzip_refl(crc, buf, len);
}

unsigned long adler32(unsigned long adler, const unsigned char *buf,
                      unsigned int len) {
  return isal_adler32(adler, buf, len);
}

struct inflate_state *InitUncompressIGZIP(int windowBits) {
  struct inflate_state *isal_strm_inflate =
      (struct inflate_state *)malloc(sizeof(struct inflate_state));
  if (!isal_strm_inflate) {
    Log(LogLevel::LOG_ERROR, "InitUncompressIGZIP() Line ", __LINE__,
        " memory allocation for inflate_state failed\n");
    return nullptr;
  }

  Log(LogLevel::LOG_INFO, "InitUncompressIGZIP() Line ", __LINE__,
      " initializing inflate with windowBits ", windowBits, "\n");

  /* Setup ISA-L decompression context */
  isal_inflate_init(isal_strm_inflate);

  isal_strm_inflate->avail_in = 0;
  isal_strm_inflate->next_in = NULL;
  // strm->total_out = 0;
  // strm->total_in = 0;

  // s->read_in_correction_applied = 0;

  ConfigureInflateWindow(isal_strm_inflate, windowBits);

  return isal_strm_inflate;
}

IGZIPNoInputAction IGZIPHandleActiveStreamNoInput(
    z_streamp strm, struct inflate_state *isal_strm_inflate, int window_bits,
    int *read_in_correction_applied, int *ret) {
  if (strm == nullptr || isal_strm_inflate == nullptr || ret == nullptr ||
      read_in_correction_applied == nullptr || strm->avail_in != 0) {
    return IGZIP_NO_INPUT_NOT_HANDLED;
  }

  uint32_t input_len = 0;
  uint32_t output_len = strm->avail_out;
  bool end_of_stream = true;

  *ret = UncompressIGZIP(isal_strm_inflate, strm->next_in, &input_len,
                         strm->next_out, &output_len, window_bits,
                         read_in_correction_applied, &strm->total_in,
                         &strm->total_out, &end_of_stream);

  if (*ret == Z_DATA_ERROR) {
    Log(LogLevel::LOG_INFO, "IGZIPHandleActiveStreamNoInput() Line ", __LINE__,
        " requested zlib fallback for raw INPUT_DONE ambiguity\n");
    return IGZIP_NO_INPUT_FALLBACK_ZLIB;
  }

  if (*ret == 0) {
    strm->next_out += output_len;
    strm->avail_out -= output_len;
    strm->total_out += output_len;
    if (output_len > 0) {
      *ret = end_of_stream ? Z_STREAM_END : Z_OK;
    } else {
      *ret = Z_BUF_ERROR;
    }
    return IGZIP_NO_INPUT_RETURN;
  }

  *ret = Z_BUF_ERROR;
  return IGZIP_NO_INPUT_RETURN;
}

IGZIPInflatePathAction IGZIPRunInflateAndSelectPathAction(
    z_streamp strm, struct inflate_state **isal_strm_inflate, int window_bits,
    int *read_in_correction_applied, uint32_t *input_length,
    uint32_t *output_length, int *ret, bool *end_of_stream,
    uint32_t pre_avail_in) {
  if (strm == nullptr || isal_strm_inflate == nullptr ||
      input_length == nullptr || output_length == nullptr || ret == nullptr ||
      end_of_stream == nullptr || read_in_correction_applied == nullptr) {
    if (ret != nullptr) {
      *ret = Z_DATA_ERROR;
    }
    return IGZIP_INFLATE_PATH_NONE;
  }

  if (*isal_strm_inflate == nullptr) {
    *isal_strm_inflate = InitUncompressIGZIP(window_bits);
    if (*isal_strm_inflate == nullptr) {
      Log(LogLevel::LOG_ERROR, "IGZIPRunInflateAndSelectPathAction() Line ",
          __LINE__, " failed to initialize igzip inflate stream\n");
      *ret = Z_DATA_ERROR;
      return IGZIP_INFLATE_PATH_NONE;
    }
  }

  *ret = UncompressIGZIP(*isal_strm_inflate, strm->next_in, input_length,
                         strm->next_out, output_length, window_bits,
                         read_in_correction_applied, &strm->total_in,
                         &strm->total_out, end_of_stream);

  const uint32_t remaining_after_igzip =
      (pre_avail_in >= *input_length) ? (pre_avail_in - *input_length) : 0;

  if (*ret == 0 && window_bits < 0 && *end_of_stream &&
      remaining_after_igzip > 0 && *read_in_correction_applied == 0 &&
      strm->total_in == 0 && strm->total_out == 0) {
    Log(LogLevel::LOG_ERROR,
        "IGZIPRunInflateAndSelectPathAction() raw boundary guard FIRED strm=",
        static_cast<void *>(strm), " bytes_in=", *input_length,
        " bytes_out=", *output_length, " pre_avail_in=", pre_avail_in,
        " remaining_in=", remaining_after_igzip, "\n");
    *ret = 1;
    *end_of_stream = false;
    return IGZIP_INFLATE_PATH_FALLBACK_RAW_BOUNDARY;
  }

  if (*ret == Z_NEED_DICT) {
    return IGZIP_INFLATE_PATH_FALLBACK_NEED_DICT;
  }
  if (*ret == Z_DATA_ERROR) {
    return IGZIP_INFLATE_PATH_FALLBACK_DATA_ERROR;
  }
  if (*ret == 0) {
    return IGZIP_INFLATE_PATH_SET_IGZIP;
  }

  return IGZIP_INFLATE_PATH_NONE;
}

int UncompressIGZIP(struct inflate_state *isal_strm_inflate, uint8_t *input,
                    uint32_t *input_length, uint8_t *output,
                    uint32_t *output_length, int window_bits,
                    int *read_in_correction_applied, unsigned long *total_in,
                    unsigned long *total_out, bool *end_of_stream) {
  (void)total_in;

  if (!isal_strm_inflate) {
    Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
        " isal_strm_inflate is NULL\n");
    return Z_STREAM_ERROR;
  }
  // set stream->avail_in, next_in, avail_out, next_out (from zstream)​
  isal_strm_inflate->next_out = output;
  const uint32_t original_avail_out = *output_length;
  isal_strm_inflate->avail_out = original_avail_out;
  const uint32_t original_avail_in = *input_length;
  isal_strm_inflate->avail_in = original_avail_in;
  isal_strm_inflate->next_in = input;
  isal_strm_inflate->total_out = *total_out;

  const int decomp = isal_inflate(isal_strm_inflate);

  uint32_t consumed_before_adjust = 0;
  if (isal_strm_inflate->avail_in <= original_avail_in) {
    consumed_before_adjust = original_avail_in - isal_strm_inflate->avail_in;
  } else {
    Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
        " invalid avail_in ", isal_strm_inflate->avail_in,
        " greater than original_avail_in ", original_avail_in,
        ", clamping consumed bytes to 0\n");
    consumed_before_adjust = 0;
  }

  uint32_t rewind_adjust_bytes = 0;

  // WORKAROUND: ISA-L raw-deflate over-consumption fix.
  // ISAL pre-loads input in 8-byte word chunks into a 64-bit shift register
  // (read_in). After BLOCK_FINISH, read_in_length >> 3 is the exact byte
  // count over-consumed, covering all avail_in scenarios: [0], [1,7], [8],
  // and >8 (multi-frame), where prior heuristics were blind or inaccurate.
  if (window_bits < 0 &&
      (decomp == ISAL_DECOMP_OK || decomp == ISAL_END_INPUT) &&
      isal_strm_inflate->block_state == ISAL_BLOCK_FINISH) {
    const uint32_t read_in_correction =
        (isal_strm_inflate->read_in_length > 0)
            ? static_cast<uint32_t>(isal_strm_inflate->read_in_length >> 3)
            : 0u;
    Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ", __LINE__,
        " raw_finish avail_in ", isal_strm_inflate->avail_in,
        " read_in_length_bits ", isal_strm_inflate->read_in_length,
        " read_in_correction_bytes ", read_in_correction, "\n");
    if (read_in_correction > 0) {
      rewind_adjust_bytes = (read_in_correction <= consumed_before_adjust)
                                ? read_in_correction
                                : consumed_before_adjust;
      *read_in_correction_applied = 1;
    }
  }

  // WORKAROUND: BLOCK_INPUT_DONE — output-buffer-limited with ambiguous
  // trailer bytes. read_in_length does not apply here (not yet at
  // BLOCK_FINISH); request caller fallback to zlib. BLOCK_FINISH is fully
  // handled above.
  if (window_bits < 0 && decomp == ISAL_DECOMP_OK &&
      *read_in_correction_applied == 0 &&
      isal_strm_inflate->block_state == ISAL_BLOCK_INPUT_DONE &&
      isal_strm_inflate->avail_in < 8 && isal_strm_inflate->avail_in > 0) {
    Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ", __LINE__,
        " raw INPUT_DONE ambiguity detected: over_consumed ",
        8u - isal_strm_inflate->avail_in, ", requesting zlib fallback\n");
    return Z_DATA_ERROR;
  }

  *output_length = original_avail_out - isal_strm_inflate->avail_out;
  *input_length = consumed_before_adjust - rewind_adjust_bytes;
  input = isal_strm_inflate->next_in;
  output = isal_strm_inflate->next_out;

  if (end_of_stream != nullptr) {
    *end_of_stream = (isal_strm_inflate->block_state == ISAL_BLOCK_FINISH);
  }

  int ret = 1;
  if (decomp == ISAL_DECOMP_OK || decomp == ISAL_END_INPUT) {
    ret = 0;
  } else if (decomp == ISAL_NEED_DICT) {
    ret = Z_NEED_DICT;
  }

  if (ret == Z_OK) {
    Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ", __LINE__,
        " inflate finished successfully Z_OK\n");
  } else if (ret == Z_STREAM_END) {
    Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ", __LINE__,
        " inflate finished with Z_STREAM_END\n");
  } else {
    Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
        " inflate finished with error code ", ret, "\n");
    switch (decomp) {
      case ISAL_INVALID_BLOCK:
        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
            " ISA-L error - Invalid block\n");
        break;
      case ISAL_INVALID_SYMBOL:
        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
            " ISA-L error - Invalid symbol\n");
        break;
      case ISAL_INVALID_LOOKBACK:
        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
            " ISA-L error - Invalid lookback\n");
        break;
      case ISAL_END_INPUT:
        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
            " ISA-L error - End of input reached unexpectedly\n");
        break;
      case ISAL_UNSUPPORTED_METHOD:
        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
            " ISA-L error - Unsupported method\n");
        break;
      case ISAL_NEED_DICT:
        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
            " ISA-L error - Need dictionary\n");
        break;
      default:
        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
            " ISA-L error code ", decomp, "\n");
        break;
    }
  }

  return ret;
}

int EndUncompressIGZIP(struct inflate_state *isal_strm_inflate) {
  if (!isal_strm_inflate) {
    Log(LogLevel::LOG_ERROR, "EndUncompressIGZIP() Line ", __LINE__,
        " z_streamp is NULL\n");
    return Z_STREAM_ERROR;
  }

  free(isal_strm_inflate);

  Log(LogLevel::LOG_INFO, "EndUncompressIGZIP() Line ", __LINE__,
      " inflate end\n");
  return Z_OK;
}

int inflateSetDictionary(z_streamp strm, unsigned char *dict_data,
                         unsigned int dict_len) {
  if (!strm || !strm->state || !dict_data || dict_len == 0)
    return Z_STREAM_ERROR;

  const inflate_state2 *s = (inflate_state2 *)strm->state;

  if (!s || !s->isal_strm_inflate) return Z_STREAM_ERROR;

  return isal_inflate_set_dict(s->isal_strm_inflate, dict_data, dict_len);
}

int ResetUncompressIGZIP(struct inflate_state *isal_strm_inflate,
                         int *read_in_correction_applied) {
  if (!isal_strm_inflate) {
    Log(LogLevel::LOG_ERROR, "ResetUncompressIGZIP() Line ", __LINE__,
        " isal_strm_inflate is NULL\n");
    return Z_STREAM_ERROR;
  }

  // Reset ISA-L inflate state
  isal_inflate_reset(isal_strm_inflate);

  *read_in_correction_applied = 0;

  return Z_OK;
}
#endif
