// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#ifdef USE_IGZIP

#include "igzip.h"
#include "crc.h"

#include <stdlib.h>
#include <string.h>
#include "logging.h"

static uint16_t ClampHistBits(int bits)
{
        if (bits < 0) {
                return 0;
        }
        if (bits > ISAL_DEF_MAX_HIST_BITS) {
                return ISAL_DEF_MAX_HIST_BITS;
        }
        return (uint16_t) bits;
}

static void ConfigureDeflateWindow(struct isal_zstream *isal_strm, int windowBits)
{
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

static void ConfigureInflateWindow(struct inflate_state *isal_strm_inflate, int windowBits)
{
        if (windowBits < 0) {
                isal_strm_inflate->crc_flag = IGZIP_DEFLATE;
                isal_strm_inflate->hist_bits = ClampHistBits(-windowBits);
                return;
        }

        if ((windowBits >= 24 && windowBits <= 31) || (windowBits >= 40 && windowBits <= 47)) {
                isal_strm_inflate->crc_flag = IGZIP_GZIP;
                isal_strm_inflate->hist_bits = ClampHistBits(windowBits > 31 ? windowBits - 32 : windowBits - 16);
                return;
        }

        isal_strm_inflate->crc_flag = IGZIP_ZLIB;
        isal_strm_inflate->hist_bits = ClampHistBits(windowBits);
}

struct isal_zstream*
InitCompressIGZIP(int level, int windowBits)
{
#ifdef DEBUG
        Log(LogLevel::LOG_INFO, "InitCompressIGZIP() Line ", __LINE__,
            " initializing deflate with level ", level, ", windowBits ",
            windowBits, "\n");
#endif
  	Log(LogLevel::LOG_INFO, "InitCompressIGZIP() Line ", __LINE__, " level ",
      	  level, ", windowBits ",windowBits, " \n");

        struct isal_zstream *isal_strm =
                (struct isal_zstream *) malloc(sizeof(struct isal_zstream));
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

/*int
deflateInit_(z_streamp strm, int level)
{
        if (!strm) {
                Log(LogLevel::LOG_ERROR, "deflateInit_() Line ", __LINE__,
                    " z_streamp is NULL\n");
                return -1;
        }

        return deflateInit2_(strm, level, 0, 15, 8, 0); // hardcoded windowBits
}*/

int
CompressIGZIP(struct isal_zstream *isal_strm, int flush, uint8_t *input, 
		uint32_t *input_length, uint8_t *output, uint32_t *output_length,
		unsigned long *total_in, unsigned long *total_out) {
        int ret;

        (void) total_in;
        (void) total_out;

  	Log(LogLevel::LOG_INFO, "CompressIGZIP() Line ", __LINE__, " input_length ",
      	  *input_length, " \n");
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

#ifdef DEBUG
        Log(LogLevel::LOG_INFO, "CompressIGZIP() Line ", __LINE__, " gzip_flag ",
            isal_strm->gzip_flag, ", hist_bits ", isal_strm->hist_bits,
            ", flush ", isal_strm->flush, ", level ", isal_strm->level,
            ", avail_in ", isal_strm->avail_in, ", avail_out ",
            (uint32_t)isal_strm->avail_out, ", total_out ",
            (uint32_t)isal_strm->total_out, ", total_in ",
            (uint32_t)isal_strm->total_in, "\n");
#endif

        int comp = isal_deflate(isal_strm);

        *output_length = original_avail_out - isal_strm->avail_out;
        *input_length = original_avail_in - isal_strm->avail_in;
        input = isal_strm->next_in;
        output = isal_strm->next_out;

#ifdef DEBUG
        Log(LogLevel::LOG_INFO, "CompressIGZIP() Line ", __LINE__,
            " after isal_deflate: avail_in ", isal_strm->avail_in,
            ", avail_out ", (uint32_t)isal_strm->avail_out,
            ", bytes_consumed ", *input_length,
            ", bytes_produced ", *output_length, "\n");
#endif

        ret = (comp == COMP_OK) ? 0 : 1;

#ifdef DEBUG
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
                        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ",
                            __LINE__, " invalid flush\n");
                        break;
                case INVALID_PARAM:
                        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ",
                            __LINE__, " invalid parameter\n");
                        break;
                case STATELESS_OVERFLOW:
                        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ",
                            __LINE__, " stateless overflow\n");
                        break;
                case ISAL_INVALID_OPERATION:
                        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ",
                            __LINE__, " invalid operation\n");
                        break;
                case ISAL_INVALID_STATE:
                        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ",
                            __LINE__, " invalid state\n");
                        break;
                case ISAL_INVALID_LEVEL:
                        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ",
                            __LINE__, " invalid level\n");
                        break;
                case ISAL_INVALID_LEVEL_BUF:
                        Log(LogLevel::LOG_ERROR, "CompressIGZIP() Line ",
                            __LINE__, " invalid level buffer\n");
                        break;
                }
        }
#endif

        return ret;
}

int
EndCompressIGZIP(struct isal_zstream* isal_strm)
{
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

#ifdef DEBUG
        Log(LogLevel::LOG_INFO, "EndCompressIGZIP() Line ", __LINE__,
            " deflate end\n");
#endif
        return Z_OK;
}

/*int
deflateSetHeader(z_streamp strm, void *head)
{
        (void) head; // Suppress unused parameter warning
        return Z_OK;
}*/

int
deflateSetDictionary(z_streamp strm, unsigned char *dict_data, unsigned int dict_len)
{
        if (!strm || !strm->state || !dict_data || dict_len == 0)
                return Z_STREAM_ERROR;

        deflate_state *s = (deflate_state *) strm->state;

        if (!s || !s->isal_strm)
                return Z_STREAM_ERROR;

        return isal_deflate_set_dict(s->isal_strm, dict_data, dict_len);
}

/*int
compress2(uint8_t *dest, unsigned long *dest_len, const uint8_t *source, unsigned long source_len,
          int level)
{
        z_stream strm;
        int err;
        const unsigned int max = (unsigned int) -1;
        unsigned long left = *dest_len;
        if (dest == NULL || dest_len == NULL || source == NULL) {
                return Z_STREAM_ERROR;
        }
        *dest_len = 0;

        strm.zalloc = NULL;
        strm.zfree = NULL;
        strm.opaque = NULL;

        err = deflateInit_(&strm, level);
        if (err != Z_OK)
                return err;

        strm.next_out = dest;
        strm.avail_out = 0;
        strm.next_in = (uint8_t *) source;
        strm.avail_in = 0;

        do {
                if (strm.avail_out == 0) {
                        strm.avail_out = left > (unsigned long) max ? max : (unsigned int) left;
                        left -= strm.avail_out;
                }
                if (strm.avail_in == 0) {
                        strm.avail_in =
                                source_len > (unsigned long) max ? max : (unsigned int) source_len;
                        source_len -= strm.avail_in;
                }
                err = deflate(&strm, source_len ? Z_NO_FLUSH : Z_FINISH);
        } while (err == Z_OK);

        *dest_len = strm.total_out;
        deflateEnd(&strm);

        return err == Z_STREAM_END ? Z_OK : err;
}

int
compress(uint8_t *dest, unsigned long *dest_len, const uint8_t *source, unsigned long source_len)
{
        return compress2(dest, dest_len, source, source_len, Z_DEFAULT_COMPRESSION);
}
*/
unsigned long
crc32(unsigned long crc, const unsigned char *buf, unsigned int len)
{
        return crc32_gzip_refl(crc, buf, len);
}

unsigned long
adler32(unsigned long adler, const unsigned char *buf, unsigned int len)
{
        return isal_adler32(adler, buf, len);
}

//inflateInit2_(z_streamp strm, int windowBits)
struct inflate_state*
InitUncompressIGZIP(int windowBits)
{
        struct inflate_state *isal_strm_inflate =
                (struct inflate_state *) malloc(sizeof(struct inflate_state));
        if (!isal_strm_inflate) {
                Log(LogLevel::LOG_ERROR, "InitUncompressIGZIP() Line ",
                    __LINE__, " memory allocation for inflate_state failed\n");
                return nullptr;
        }

#ifdef DEBUG
        Log(LogLevel::LOG_INFO, "InitUncompressIGZIP() Line ", __LINE__,
            " initializing inflate with windowBits ", windowBits, "\n");
#endif
  	Log(LogLevel::LOG_INFO, "InitUncompressIGZIP() Line ", __LINE__, 
      	  ", windowBits ",windowBits, " \n");

        /* Setup ISA-L decompression context */
        isal_inflate_init(isal_strm_inflate);

        isal_strm_inflate->avail_in = 0;
        isal_strm_inflate->next_in = NULL;
        //strm->total_out = 0;
        //strm->total_in = 0;

        //s->trailer_overconsumption_fixed = 0; // Initialize the workaround flag

        ConfigureInflateWindow(isal_strm_inflate, windowBits);

        return isal_strm_inflate;
}

/*int
inflateInit_(z_streamp strm)
{
        if (!strm) {
                Log(LogLevel::LOG_ERROR, "inflateInit_() Line ", __LINE__,
                    " z_streamp is NULL\n");
                return Z_STREAM_ERROR;
        }

        return inflateInit2_(strm, 15); // hardcoded windowBits
}*/

int
UncompressIGZIP(struct inflate_state *isal_strm_inflate, uint8_t *input,
		uint32_t *input_length, uint8_t *output, uint32_t *output_length,
                int *tofixed, unsigned long *total_in, unsigned long *total_out,
                bool *end_of_stream)
{
        (void) total_in;

        if (!isal_strm_inflate) {
                Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ", __LINE__,
                    " isal_strm_inflate is NULL\n");
                return Z_STREAM_ERROR;
        }

  	Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ", __LINE__,
  		 "input length ", *input_length,"\n");
        // set stream->avail_in, next_in, avail_out, next_out (from zstream)​
        isal_strm_inflate->next_out = output;
        const uint32_t original_avail_out = *output_length;
        isal_strm_inflate->avail_out = original_avail_out;
        const uint32_t original_avail_in = *input_length;
        isal_strm_inflate->avail_in = original_avail_in;
        isal_strm_inflate->next_in = input;
        isal_strm_inflate->total_out = *total_out;

        Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ", __LINE__,
        	" CRC flag: ", (uint32_t)isal_strm_inflate->crc_flag,
        	" Before isal_inflate: avail_in ",isal_strm_inflate->avail_in,
		//" next_in= ", isal_strm_inflate->next_in, 
		" avail_out= ",(uint32_t)isal_strm_inflate->avail_out, 
		//" next_out= ", isal_strm_inflate->next_out,
        	" Total out: ", (uint32_t)isal_strm_inflate->total_out, 
		" Total_in: ", (unsigned long)*total_in);

        const int decomp = isal_inflate(isal_strm_inflate);

        Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ", __LINE__,
        	" After isal_inflate: avail_in ",isal_strm_inflate->avail_in,
		//" next_in= ", isal_strm_inflate->next_in, 
		" avail_out= ",(uint32_t)isal_strm_inflate->avail_out, 
		//" next_out= ", isal_strm_inflate->next_out,
        	" Total out: ", (uint32_t)isal_strm_inflate->total_out, 
                " Total_in: ", (unsigned long)*total_in +
                        (original_avail_in - isal_strm_inflate->avail_in),
                " Bytes consumed this call: ",
                        (original_avail_in - isal_strm_inflate->avail_in),
        	" Block state: ", isal_strm_inflate->block_state, " (ISAL_BLOCK_FINISH=", 
                ISAL_BLOCK_FINISH, " ISA-L result: ", decomp, "\n");

        if (isal_strm_inflate->block_state == ISAL_BLOCK_FINISH &&
            isal_strm_inflate->avail_in > 0) {
                Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ", __LINE__,
                " WARNING: BLOCK_FINISH reached but ",isal_strm_inflate->avail_in,
                " bytes remain in input\n");
        }

        // WORKAROUND: ISA-L over-consumption fix for raw deflate mode
        if ((isal_strm_inflate->block_state == ISAL_BLOCK_FINISH ||
             isal_strm_inflate->block_state == ISAL_BLOCK_INPUT_DONE) &&
                        (isal_strm_inflate->crc_flag == IGZIP_DEFLATE) &&
            *tofixed == 0 && // hasn't been applied yet
            decomp == ISAL_DECOMP_OK &&              // successful decompression
            isal_strm_inflate->avail_in < 8 && isal_strm_inflate->avail_in > 0) {

                // Calculate how many bytes were likely over-consumed
                const unsigned int expected_trailer_bytes = 8;
                const unsigned int over_consumed =
                        expected_trailer_bytes - isal_strm_inflate->avail_in;

                // Only apply fix if the over-consumption is reasonable (1-7 bytes)
                if (over_consumed >= 1 && over_consumed <= 7) {
#ifdef DEBUG
                                                Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ",
                                                        __LINE__, " applying workaround: detected ISA-L over-consumption of ",
                                                        over_consumed, " bytes\n");
                                                Log(LogLevel::LOG_INFO, "UncompressIGZIP() Line ",
                                                        __LINE__, " adjusting avail_in from ",
                                                        isal_strm_inflate->avail_in, " to ",
                                                        isal_strm_inflate->avail_in + over_consumed,
                                                        "\n");
#endif
                        // Rewind the input pointer to restore over-consumed bytes
                        isal_strm_inflate->next_in =
                                (unsigned char *) isal_strm_inflate->next_in - over_consumed;
                        isal_strm_inflate->avail_in += over_consumed;

                        // Mark that the workaround has been applied
                        *tofixed = 1;

                        // Also adjust the byte consumption count to reflect the actual deflate data
                        // consumed Note: bytes_consumed is calculated later, so we'll need to
                        // adjust it after the calculation
                }
        }

        *output_length = original_avail_out - isal_strm_inflate->avail_out;
        *input_length = original_avail_in - isal_strm_inflate->avail_in;
        input = isal_strm_inflate->next_in;
        output = isal_strm_inflate->next_out;

        if (end_of_stream != nullptr) {
                *end_of_stream = (isal_strm_inflate->block_state == ISAL_BLOCK_FINISH);
        }

        int ret = (decomp == ISAL_DECOMP_OK || decomp == ISAL_END_INPUT) ? 0 : 1;

#ifdef DEBUG
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
                        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ",
                            __LINE__, " ISA-L error - Invalid block\n");
                        break;
                case ISAL_INVALID_SYMBOL:
                        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ",
                            __LINE__, " ISA-L error - Invalid symbol\n");
                        break;
                case ISAL_INVALID_LOOKBACK:
                        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ",
                            __LINE__, " ISA-L error - Invalid lookback\n");
                        break;
                case ISAL_END_INPUT:
                        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ",
                            __LINE__, " ISA-L error - End of input reached unexpectedly\n");
                        break;
                case ISAL_UNSUPPORTED_METHOD:
                        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ",
                            __LINE__, " ISA-L error - Unsupported method\n");
                        break;
                case ISAL_NEED_DICT:
                        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ",
                            __LINE__, " ISA-L error - Need dictionary\n");
                        break;
                default:
                        Log(LogLevel::LOG_ERROR, "UncompressIGZIP() Line ",
                            __LINE__, " ISA-L error code ", decomp, "\n");
                        break;
                }
        }
#endif

        return ret;
}

int
EndUncompressIGZIP(struct inflate_state *isal_strm_inflate)
{
        if (!isal_strm_inflate) {
                Log(LogLevel::LOG_ERROR, "EndUncompressIGZIP() Line ",
                    __LINE__, " z_streamp is NULL\n");
                return Z_STREAM_ERROR;
        }

                free(isal_strm_inflate);


#ifdef DEBUG
        Log(LogLevel::LOG_INFO, "EndUncompressIGZIP() Line ", __LINE__,
            " inflate end\n");
#endif
        return Z_OK;
}

int
inflateSetDictionary(z_streamp strm, unsigned char *dict_data, unsigned int dict_len)
{
        if (!strm || !strm->state || !dict_data || dict_len == 0)
                return Z_STREAM_ERROR;

        const inflate_state2 *s = (inflate_state2 *) strm->state;

        if (!s || !s->isal_strm_inflate)
                return Z_STREAM_ERROR;

        return isal_inflate_set_dict(s->isal_strm_inflate, dict_data, dict_len);
}

/*int
uncompress2(uint8_t *dest, unsigned long *dest_len, const uint8_t *source,
            unsigned long *source_len)
{
        z_stream strm;
        int err;
        const unsigned int max = (unsigned int) -1;
        unsigned long len, left;
        uint8_t buf[1] = { 0 }; // for detection of incomplete strm when *dest_len == 0 

        len = *source_len;
        if (*dest_len) {
                left = *dest_len;
                *dest_len = 0;
        } else {
                left = 1;
                dest = buf;
        }

        strm.next_in = (uint8_t *) source;
        strm.avail_in = 0;
        strm.zalloc = NULL;
        strm.zfree = NULL;
        strm.opaque = NULL;

        err = inflateInit_(&strm);
        if (err != Z_OK)
                return err;

        strm.next_out = dest;
        strm.avail_out = 0;

        do {
                if (strm.avail_out == 0) {
                        strm.avail_out = left > (unsigned long) max ? max : (unsigned int) left;
                        left -= strm.avail_out;
                }
                if (strm.avail_in == 0) {
                        strm.avail_in = len > (unsigned long) max ? max : (unsigned int) len;
                        len -= strm.avail_in;
                }
                err = inflate(&strm, Z_NO_FLUSH);
        } while (err == Z_OK);

        *source_len -= len + strm.avail_in;
        if (dest != buf)
                *dest_len = strm.total_out;
        else if (strm.total_out && err == Z_BUF_ERROR)
                left = 1;

        inflateEnd(&strm);
        return err == Z_STREAM_END                           ? Z_OK
               : err == Z_NEED_DICT                          ? Z_DATA_ERROR
               : err == Z_BUF_ERROR && left + strm.avail_out ? Z_DATA_ERROR
                                                             : err;
}

int
uncompress(uint8_t *dest, unsigned long *dest_len, const uint8_t *source, unsigned long source_len)
{
        return uncompress2(dest, dest_len, source, &source_len);
}*/

int
ResetUncompressIGZIP(struct inflate_state *isal_strm_inflate, int *tofixed)
{
        if (!isal_strm_inflate) {
                Log(LogLevel::LOG_ERROR, "ResetUncompressIGZIP() Line ",
                    __LINE__, " isal_strm_inflate is NULL\n");
                return Z_STREAM_ERROR;
        }

        // Reset ISA-L inflate state
        isal_inflate_reset(isal_strm_inflate);


        // Reset workaround flag
        *tofixed = 0;

        return Z_OK;
}
#endif
