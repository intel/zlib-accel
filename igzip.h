// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef USE_IGZIP
#include <igzip_lib.h>
#include <zlib.h>

typedef struct internal_state2 {
        z_streamp strm;
        int level;
        int w_bits;
        struct inflate_state *isal_strm_inflate;
        int trailer_overconsumption_fixed; /* Indicates if fix has been applied for gzip trailer
                                              overconsumption issue */
} inflate_state2;

typedef struct internal_state {
        z_streamp strm;
        int level;
        int w_bits;
        struct isal_zstream *isal_strm;
} deflate_state;

struct isal_zstream*
InitCompressIGZIP(int level, int windowBits);
int
CompressIGZIP(struct isal_zstream *isal_strm, int flush, uint8_t *input, 
		uint32_t* input_length, uint8_t *output, uint32_t* output_length,
		unsigned long *total_in, unsigned long *total_out); 
bool
IsIGZIPDeflateFinished(const struct isal_zstream* stream);
bool
SupportedOptionsIGZIPCompress(int flush, uint32_t output_length,
                              bool stream_on_igzip_path);
bool
SupportedOptionsIGZIPUncompress(int window_bits, uint32_t input_length,
                                uint32_t output_length,
                                bool stream_on_igzip_path);
enum IGZIPNoInputAction {
        IGZIP_NO_INPUT_NOT_HANDLED,
        IGZIP_NO_INPUT_RETURN,
        IGZIP_NO_INPUT_FALLBACK_ZLIB,
};

enum IGZIPInflatePathAction {
        IGZIP_INFLATE_PATH_NONE,
        IGZIP_INFLATE_PATH_SET_IGZIP,
        IGZIP_INFLATE_PATH_FALLBACK_NEED_DICT,
        IGZIP_INFLATE_PATH_FALLBACK_DATA_ERROR,
        IGZIP_INFLATE_PATH_FALLBACK_RAW_BOUNDARY,
};

IGZIPNoInputAction
IGZIPHandleActiveStreamNoInput(z_streamp strm,
                               struct inflate_state *isal_strm_inflate,
                               int window_bits, int *tofixed, int *ret);

IGZIPInflatePathAction
IGZIPRunInflateAndSelectPathAction(z_streamp strm,
                                   struct inflate_state **isal_strm_inflate,
                                   int window_bits, int *tofixed,
                                   uint32_t *input_length,
                                   uint32_t *output_length, int *ret,
                                   bool *end_of_stream,
                                   uint32_t pre_avail_in);

bool
IGZIPShouldFallbackDeflate(bool stream_on_igzip_path, int flush,
                           uint32_t avail_in);
int
EndCompressIGZIP(struct isal_zstream* isal_strm);

struct inflate_state*
InitUncompressIGZIP(int windowBits);
int
UncompressIGZIP(struct inflate_state *isal_strm_inflate, uint8_t *input,
		uint32_t *input_length, uint8_t *output, uint32_t *output_length,
                int window_bits, int *tofixed,
                unsigned long *total_in, unsigned long *total_out,
                bool *end_of_stream);
int
EndUncompressIGZIP(struct inflate_state *isal_strm_inflate);
int
ResetUncompressIGZIP(struct inflate_state *isal_strm_inflate, int *tofixed);
//#define Z_DEFAULT_COMPRESSION 6
#endif
