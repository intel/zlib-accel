// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#ifdef USE_IGZIP

#include "igzip.h"

#include <stdlib.h>
#include <string.h>

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

struct isal_zstream *InitCompressIGZIP(int level, int windowBits) {
  Log(LogLevel::LOG_INFO,
      "InitCompressIGZIP() initializing deflate with level ", level,
      ", windowBits ", windowBits, "\n");

  struct isal_zstream *isal_strm =
      (struct isal_zstream *)malloc(sizeof(struct isal_zstream));
  if (!isal_strm) {
    Log(LogLevel::LOG_ERROR,
        "InitCompressIGZIP() memory allocation for isal_zstream failed\n");
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
    Log(LogLevel::LOG_ERROR, "InitCompressIGZIP() invalid compression level\n");
    free(isal_strm);
    return nullptr;
  }

  if (!isal_strm->level_buf) {
    free(isal_strm);
    Log(LogLevel::LOG_ERROR,
        "InitCompressIGZIP() memory allocation for level_buf failed\n");
    return nullptr;
  }

  ConfigureDeflateWindow(isal_strm, windowBits);

  return isal_strm;
}

int CompressIGZIP(struct isal_zstream *isal_strm, int flush,
                  const uint8_t *input, uint32_t *input_length, uint8_t *output,
                  uint32_t *output_length, const unsigned long *total_in,
                  const unsigned long *total_out) {
  int ret;

  if (!isal_strm) {
    Log(LogLevel::LOG_ERROR, "CompressIGZIP() deflate isal_strm is NULL\n");
    return -1;
  }

  // set stream->avail_in, next_in, avail_out, next_out (from zstream)​
  isal_strm->next_out = output;
  const uint32_t original_avail_out = *output_length;
  isal_strm->avail_out = original_avail_out;
  isal_strm->next_in = const_cast<uint8_t *>(input);
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
      Log(LogLevel::LOG_ERROR, "CompressIGZIP() invalid flush value\n");
      return -1;
  }

  Log(LogLevel::LOG_INFO, "CompressIGZIP() gzip_flag ", isal_strm->gzip_flag,
      ", hist_bits ", isal_strm->hist_bits, ", flush ", isal_strm->flush,
      ", level ", isal_strm->level, ", avail_in ", isal_strm->avail_in,
      ", avail_out ", (uint32_t)isal_strm->avail_out, ", total_out ",
      (uint32_t)isal_strm->total_out, ", total_in ",
      (uint32_t)isal_strm->total_in, "\n");

  // ISA-L always emits sync bytes on SYNC_FLUSH regardless of pending data.
  // When the stream is already byte-aligned (ZSTATE_NEW_HDR) and there is no
  // new input, no real progress can be made — return 0 progress so the caller
  // reports Z_BUF_ERROR, matching zlib's semantics for empty flush calls.
  // ZSTATE_NEW_HDR is the idle/byte-aligned state in ISA-L's internal deflate
  // state machine; validated against ISA-L v2.32.0 (commit c196241).
  if (isal_strm->avail_in == 0 && isal_strm->flush == SYNC_FLUSH &&
      isal_strm->end_of_stream == 0 &&
      isal_strm->internal_state.state == ZSTATE_NEW_HDR) {
    *output_length = 0;
    *input_length = 0;
    return 0;
  }

  int comp = isal_deflate(isal_strm);

  *output_length = original_avail_out - isal_strm->avail_out;
  *input_length = original_avail_in - isal_strm->avail_in;

  Log(LogLevel::LOG_INFO, "CompressIGZIP() after isal_deflate: avail_in ",
      isal_strm->avail_in, ", avail_out ", (uint32_t)isal_strm->avail_out,
      ", bytes_consumed ", *input_length, ", bytes_produced ", *output_length,
      "\n");

  ret = (comp == COMP_OK) ? Z_OK : Z_STREAM_ERROR;

  if (ret == Z_OK) {
    Log(LogLevel::LOG_INFO,
        "CompressIGZIP() deflate finished successfully Z_OK\n");
  } else if (ret == Z_STREAM_END) {
    Log(LogLevel::LOG_INFO,
        "CompressIGZIP() deflate finished successfully Z_STREAM_END\n");
  } else {
    Log(LogLevel::LOG_ERROR,
        "CompressIGZIP() deflate finished with error code ", ret, "\n");
    switch (comp) {
      case INVALID_FLUSH:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() invalid flush\n");
        break;
      case INVALID_PARAM:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() invalid parameter\n");
        break;
      case STATELESS_OVERFLOW:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() stateless overflow\n");
        break;
      case ISAL_INVALID_OPERATION:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() invalid operation\n");
        break;
      case ISAL_INVALID_STATE:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() invalid state\n");
        break;
      case ISAL_INVALID_LEVEL:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() invalid level\n");
        break;
      case ISAL_INVALID_LEVEL_BUF:
        Log(LogLevel::LOG_ERROR, "CompressIGZIP() invalid level buffer\n");
        break;
    }
  }

  return ret;
}

int EndCompressIGZIP(struct isal_zstream *isal_strm) {
  if (!isal_strm) {
    Log(LogLevel::LOG_ERROR, "EndCompressIGZIP() isal_stream is NULL\n");
    return -1;
  }

  // Free allocated memory for level_buf and isal_strm
  if (isal_strm->level_buf) {
    free(isal_strm->level_buf);
  }
  free(isal_strm);

  Log(LogLevel::LOG_INFO, "EndCompressIGZIP() deflate end\n");
  return Z_OK;
}

struct inflate_state *InitUncompressIGZIP(int windowBits) {
  struct inflate_state *isal_strm_inflate =
      (struct inflate_state *)malloc(sizeof(struct inflate_state));
  if (!isal_strm_inflate) {
    Log(LogLevel::LOG_ERROR,
        "InitUncompressIGZIP() memory allocation for inflate_state failed\n");
    return nullptr;
  }

  Log(LogLevel::LOG_INFO,
      "InitUncompressIGZIP() initializing inflate with windowBits ", windowBits,
      "\n");

  /* Setup ISA-L decompression context */
  isal_inflate_init(isal_strm_inflate);

  isal_strm_inflate->avail_in = 0;
  isal_strm_inflate->next_in = NULL;

  ConfigureInflateWindow(isal_strm_inflate, windowBits);

  return isal_strm_inflate;
}

IGZIPNoInputAction IGZIPHandleActiveStreamNoInput(
    z_streamp strm, struct inflate_state *isal_strm_inflate, int *ret) {
  // Caller guarantees strm->avail_in == 0 before calling this function.
  if (strm == nullptr || isal_strm_inflate == nullptr || ret == nullptr) {
    return IGZIP_NO_INPUT_NOT_HANDLED;
  }

  uint32_t input_len = 0;
  uint32_t output_len = strm->avail_out;
  bool end_of_stream = true;

  *ret = UncompressIGZIP(isal_strm_inflate, strm->next_in, &input_len,
                         strm->next_out, &output_len, &strm->total_in,
                         &strm->total_out, &end_of_stream);

  if (*ret == 0) {
    strm->next_out += output_len;
    strm->avail_out -= output_len;
    // This is the only site that updates total_out for the avail_in==0 path.
    // The caller returns immediately on IGZIP_NO_INPUT_RETURN, so the main
    // inflate() update block is never reached — no double-counting.
    strm->total_out += output_len;
    if (end_of_stream) {
      *ret = Z_STREAM_END;
    } else if (output_len > 0) {
      *ret = Z_OK;
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
    uint32_t *input_length, uint32_t *output_length, int *ret,
    bool *end_of_stream) {
  if (strm == nullptr || isal_strm_inflate == nullptr ||
      input_length == nullptr || output_length == nullptr || ret == nullptr ||
      end_of_stream == nullptr) {
    if (ret != nullptr) {
      *ret = Z_DATA_ERROR;
    }
    return IGZIP_INFLATE_PATH_NONE;
  }

  if (*isal_strm_inflate == nullptr) {
    *isal_strm_inflate = InitUncompressIGZIP(window_bits);
    if (*isal_strm_inflate == nullptr) {
      Log(LogLevel::LOG_ERROR,
          "IGZIPRunInflateAndSelectPathAction() failed to initialize igzip "
          "inflate stream\n");
      *ret = Z_DATA_ERROR;
      return IGZIP_INFLATE_PATH_NONE;
    }
  }

  *ret = UncompressIGZIP(*isal_strm_inflate, strm->next_in, input_length,
                         strm->next_out, output_length, &strm->total_in,
                         &strm->total_out, end_of_stream);

  // Raw boundary guard removed: ISA-L PR#215 (cd72fd7) fixes avail_in
  // over-consumption at BLOCK_FINISH in isal_inflate; the guard is no
  // longer needed for that case.

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

int UncompressIGZIP(struct inflate_state *isal_strm_inflate,
                    const uint8_t *input, uint32_t *input_length,
                    uint8_t *output, uint32_t *output_length,
                    const unsigned long *total_in,
                    const unsigned long *total_out, bool *end_of_stream) {
  (void)total_in;

  if (!isal_strm_inflate) {
    Log(LogLevel::LOG_ERROR, "UncompressIGZIP() isal_strm_inflate is NULL\n");
    return Z_STREAM_ERROR;
  }
  // set stream->avail_in, next_in, avail_out, next_out (from zstream)​
  isal_strm_inflate->next_out = output;
  const uint32_t original_avail_out = *output_length;
  isal_strm_inflate->avail_out = original_avail_out;
  const uint32_t original_avail_in = *input_length;
  isal_strm_inflate->avail_in = original_avail_in;
  isal_strm_inflate->next_in = const_cast<uint8_t *>(input);
  isal_strm_inflate->total_out = *total_out;

  const int decomp = isal_inflate(isal_strm_inflate);

  uint32_t consumed_before_adjust = 0;
  if (isal_strm_inflate->avail_in <= original_avail_in) {
    consumed_before_adjust = original_avail_in - isal_strm_inflate->avail_in;
  } else {
    Log(LogLevel::LOG_ERROR, "UncompressIGZIP() invalid avail_in ",
        isal_strm_inflate->avail_in, " greater than original_avail_in ",
        original_avail_in, ", clamping consumed bytes to 0\n");
    consumed_before_adjust = 0;
  }

  // Bug 2 guard removed: ISA-L PR#215 (cd72fd7) makes BLOCK_FINISH correctly
  // restore avail_in after over-consumption into read_in. Continuing past
  // BLOCK_INPUT_DONE (even with avail_in 1-7) reaches BLOCK_FINISH with the
  // correct stream boundary. No fallback needed.
  // (ISA-L >= 2.32.1 enforced at configure time via common.cmake.)

  *output_length = original_avail_out - isal_strm_inflate->avail_out;
  *input_length = consumed_before_adjust;

  if (end_of_stream != nullptr) {
    *end_of_stream = (isal_strm_inflate->block_state == ISAL_BLOCK_FINISH);
  }

  int ret =
      Z_DATA_ERROR;  // default: any unhandled ISA-L error maps to Z_DATA_ERROR
  if (decomp == ISAL_DECOMP_OK || decomp == ISAL_END_INPUT) {
    ret = 0;
  } else if (decomp == ISAL_NEED_DICT) {
    ret = Z_NEED_DICT;
  }

  if (ret == Z_OK) {
    Log(LogLevel::LOG_INFO,
        "UncompressIGZIP() inflate finished successfully Z_OK\n");
  } else if (ret == Z_STREAM_END) {
    Log(LogLevel::LOG_INFO,
        "UncompressIGZIP() inflate finished with Z_STREAM_END\n");
  } else {
    Log(LogLevel::LOG_ERROR,
        "UncompressIGZIP() inflate finished with error code ", ret, "\n");
    switch (decomp) {
      case ISAL_INVALID_BLOCK:
        Log(LogLevel::LOG_ERROR,
            "UncompressIGZIP() ISA-L error - Invalid block\n");
        break;
      case ISAL_INVALID_SYMBOL:
        Log(LogLevel::LOG_ERROR,
            "UncompressIGZIP() ISA-L error - Invalid symbol\n");
        break;
      case ISAL_INVALID_LOOKBACK:
        Log(LogLevel::LOG_ERROR,
            "UncompressIGZIP() ISA-L error - Invalid lookback\n");
        break;
      case ISAL_END_INPUT:
        Log(LogLevel::LOG_ERROR,
            "UncompressIGZIP() ISA-L error - End of input reached "
            "unexpectedly\n");
        break;
      case ISAL_UNSUPPORTED_METHOD:
        Log(LogLevel::LOG_ERROR,
            "UncompressIGZIP() ISA-L error - Unsupported method\n");
        break;
      case ISAL_NEED_DICT:
        Log(LogLevel::LOG_ERROR,
            "UncompressIGZIP() ISA-L error - Need dictionary\n");
        break;
      default:
        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() ISA-L error code ", decomp,
            "\n");
        break;
    }
  }

  return ret;
}

int EndUncompressIGZIP(struct inflate_state *isal_strm_inflate) {
  if (!isal_strm_inflate) {
    Log(LogLevel::LOG_ERROR, "EndUncompressIGZIP() z_streamp is NULL\n");
    return Z_STREAM_ERROR;
  }

  free(isal_strm_inflate);

  Log(LogLevel::LOG_INFO, "EndUncompressIGZIP() inflate end\n");
  return Z_OK;
}

void ResetCompressIGZIP(struct isal_zstream *isal_strm) {
  // ISA-L PR#215 (30e90b4) stops gzip_flag from being mutated during
  // compression, so isal_deflate_reset preserves it correctly.  No manual
  // ConfigureDeflateWindow call is needed here.
  isal_deflate_reset(isal_strm);
  isal_strm->end_of_stream = 0;
  isal_strm->flush = NO_FLUSH;
}

int ResetUncompressIGZIP(struct inflate_state *isal_strm_inflate) {
  if (!isal_strm_inflate) {
    Log(LogLevel::LOG_ERROR,
        "ResetUncompressIGZIP() isal_strm_inflate is NULL\n");
    return Z_STREAM_ERROR;
  }

  isal_inflate_reset(isal_strm_inflate);

  return Z_OK;
}
#endif
