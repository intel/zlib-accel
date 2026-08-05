// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef USE_IGZIP
#include <igzip_lib.h>
#include <zlib.h>

#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

struct isal_zstream *InitCompressIGZIP(int level, int windowBits);
int CompressIGZIP(struct isal_zstream *isal_strm, int flush,
                  const uint8_t *input, uint32_t *input_length, uint8_t *output,
                  uint32_t *output_length, const unsigned long *total_in,
                  const unsigned long *total_out);
bool IsIGZIPDeflateFinished(const struct isal_zstream *stream);
VISIBLE_FOR_TESTING bool SupportedOptionsIGZIPDeflate(int flush);
bool SupportedOptionsIGZIPInflate(int window_bits);

enum IGZIPInflatePathAction {
  IGZIP_INFLATE_PATH_NONE,
  IGZIP_INFLATE_PATH_SET_IGZIP,
  IGZIP_INFLATE_PATH_FALLBACK_NEED_DICT,
  IGZIP_INFLATE_PATH_FALLBACK_DATA_ERROR,
};

void IGZIPHandleActiveStreamNoInput(z_streamp strm,
                                    struct inflate_state *isal_strm_inflate,
                                    int *ret);

IGZIPInflatePathAction IGZIPRunInflateAndSelectPathAction(
    z_streamp strm, struct inflate_state **isal_strm_inflate, int window_bits,
    uint32_t *input_length, uint32_t *output_length, int *ret,
    bool *end_of_stream);

int EndCompressIGZIP(struct isal_zstream *isal_strm);
void ResetCompressIGZIP(struct isal_zstream *isal_strm);

struct inflate_state *InitUncompressIGZIP(int windowBits);
int UncompressIGZIP(struct inflate_state *isal_strm_inflate,
                    const uint8_t *input, uint32_t *input_length,
                    uint8_t *output, uint32_t *output_length,
                    const unsigned long *total_in,
                    const unsigned long *total_out, bool *end_of_stream);
int EndUncompressIGZIP(struct inflate_state *isal_strm_inflate);
int ResetUncompressIGZIP(struct inflate_state *isal_strm_inflate);
#endif
