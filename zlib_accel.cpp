// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "zlib_accel.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/param.h>
#include <unistd.h>

#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "config/config.h"
#include "logging.h"
#include "sharded_map.h"
#ifdef USE_IAA
#include "iaa.h"
#endif
#ifdef USE_QAT
#include "qat.h"
#endif

// Disable cfi-icall as it makes calls to orig* functions fail
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi-icall"))), \
                             apply_to = function)
#endif

// Original zlib functions
static int (*orig_deflateInit_)(z_streamp strm, int level, const char* version,
                                int stream_size);
static int (*orig_deflateInit2_)(z_streamp strm, int level, int method,
                                 int window_bits, int mem_level, int strategy,
                                 const char* version, int stream_size);
static int (*orig_deflate)(z_streamp strm, int flush);
static int (*orig_deflateEnd)(z_streamp strm);
static int (*orig_deflateReset)(z_streamp strm);
static int (*orig_inflateInit_)(z_streamp strm, const char* version,
                                int stream_size);
static int (*orig_inflateInit2_)(z_streamp strm, int window_bits,
                                 const char* version, int stream_size);
static int (*orig_inflate)(z_streamp strm, int flush);
static int (*orig_inflateEnd)(z_streamp strm);
static int (*orig_inflateReset)(z_streamp strm);
static int (*orig_compress)(Bytef* dest, uLongf* destLen, const Bytef* source,
                            uLong sourceLen);
static int (*orig_compress2)(Bytef* dest, uLongf* destLen, const Bytef* source,
                             uLong sourceLen, int level);
static int (*orig_uncompress)(Bytef* dest, uLongf* destLen, const Bytef* source,
                              uLong sourceLen);
static int (*orig_uncompress2)(Bytef* dest, uLongf* destLen,
                               const Bytef* source, uLong* sourceLen);
static gzFile (*orig_gzopen)(const char* path, const char* mode);
static gzFile (*orig_gzdopen)(int fd, const char* mode);
static int (*orig_gzwrite)(gzFile file, voidpc buf, unsigned len);
static int (*orig_gzread)(gzFile file, voidp buf, unsigned len);
static int (*orig_gzclose)(gzFile file);
static int (*orig_gzeof)(gzFile file);

// Initialize/cleanup functions when library is loaded
static int init_zlib_accel(void) __attribute__((constructor));
static void cleanup_zlib_accel(void) __attribute__((destructor));

static int init_zlib_accel(void) {
  orig_deflateInit_ =
      reinterpret_cast<int (*)(z_streamp, int, const char*, int)>(
          dlsym(RTLD_NEXT, "deflateInit_"));
  orig_deflateInit2_ =
      reinterpret_cast<int (*)(z_streamp, int, int, int, int, int, const char*,
                               int)>(dlsym(RTLD_NEXT, "deflateInit2_"));
  orig_deflate =
      reinterpret_cast<int (*)(z_streamp, int)>(dlsym(RTLD_NEXT, "deflate"));
  orig_deflateEnd =
      reinterpret_cast<int (*)(z_streamp)>(dlsym(RTLD_NEXT, "deflateEnd"));
  orig_deflateReset =
      reinterpret_cast<int (*)(z_streamp)>(dlsym(RTLD_NEXT, "deflateReset"));
  orig_inflateInit_ = reinterpret_cast<int (*)(z_streamp, const char*, int)>(
      dlsym(RTLD_NEXT, "inflateInit_"));
  orig_inflateInit2_ =
      reinterpret_cast<int (*)(z_streamp, int, const char*, int)>(
          dlsym(RTLD_NEXT, "inflateInit2_"));
  orig_inflate =
      reinterpret_cast<int (*)(z_streamp, int)>(dlsym(RTLD_NEXT, "inflate"));
  orig_inflateEnd =
      reinterpret_cast<int (*)(z_streamp)>(dlsym(RTLD_NEXT, "inflateEnd"));
  orig_inflateReset =
      reinterpret_cast<int (*)(z_streamp)>(dlsym(RTLD_NEXT, "inflateReset"));
  orig_compress =
      reinterpret_cast<int (*)(Bytef*, uLongf*, const Bytef*, uLong)>(
          dlsym(RTLD_NEXT, "compress"));
  orig_compress2 =
      reinterpret_cast<int (*)(Bytef*, uLongf*, const Bytef*, uLong, int)>(
          dlsym(RTLD_NEXT, "compress2"));
  orig_uncompress =
      reinterpret_cast<int (*)(Bytef*, uLongf*, const Bytef*, uLong)>(
          dlsym(RTLD_NEXT, "uncompress"));
  orig_uncompress2 =
      reinterpret_cast<int (*)(Bytef*, uLongf*, const Bytef*, uLong*)>(
          dlsym(RTLD_NEXT, "uncompress2"));
  orig_gzopen = reinterpret_cast<gzFile (*)(const char*, const char*)>(
      dlsym(RTLD_NEXT, "gzopen"));
  orig_gzdopen = reinterpret_cast<gzFile (*)(int, const char*)>(
      dlsym(RTLD_NEXT, "gzdopen"));
  orig_gzwrite = reinterpret_cast<int (*)(gzFile, voidpc, unsigned)>(
      dlsym(RTLD_NEXT, "gzwrite"));
  orig_gzread = reinterpret_cast<int (*)(gzFile, voidp, unsigned)>(
      dlsym(RTLD_NEXT, "gzread"));
  orig_gzclose = reinterpret_cast<int (*)(gzFile)>(dlsym(RTLD_NEXT, "gzclose"));
  orig_gzeof = reinterpret_cast<int (*)(gzFile)>(dlsym(RTLD_NEXT, "gzeof"));

  std::string config_file_content;
  config::LoadConfigFile(config_file_content);

#ifdef DEBUG_LOG
  if (!config::log_file.empty()) {
    CreateLogFile(config::log_file.c_str());
  }
#endif
  return 0;
}
static void cleanup_zlib_accel(void) {
#ifdef DEBUG_LOG
  CloseLogFile();
#endif
}

// Avoid recursive call (e.g., if QATzip falls back to zlib internally)
static thread_local bool in_call = false;

struct DeflateSettings {
  DeflateSettings(int _level, int _method, int _window_bits, int _mem_level,
                  int _strategy)
      : level(_level),
        method(_method),
        window_bits(_window_bits),
        mem_level(_mem_level),
        strategy(_strategy) {}

  int level;
  int method;
  int window_bits;
  int mem_level;
  int strategy;
  ExecutionPath path = UNDEFINED;
};

struct InflateSettings {
  InflateSettings(int _window_bits) : window_bits(_window_bits) {}
  int window_bits;
  ExecutionPath path = UNDEFINED;
};

class DeflateStreamSettings {
 public:
  void Set(z_streamp strm, int level, int method, int window_bits,
           int mem_level, int strategy) {
    DeflateSettings* settings =
        new DeflateSettings(level, method, window_bits, mem_level, strategy);
    map.Set(strm, settings);
  }

  void Unset(z_streamp strm) { map.Unset(strm); }

  DeflateSettings* Get(z_streamp strm) { return map.Get(strm); }

 private:
  ShardedMap<z_streamp, DeflateSettings*> map;
};
DeflateStreamSettings deflate_stream_settings;

class InflateStreamSettings {
 public:
  void Set(z_streamp strm, int window_bits) {
    InflateSettings* settings = new InflateSettings(window_bits);
    map.Set(strm, settings);
  }

  void Unset(z_streamp strm) { map.Unset(strm); }

  InflateSettings* Get(z_streamp strm) { return map.Get(strm); }

 private:
  ShardedMap<z_streamp, InflateSettings*> map;
};
InflateStreamSettings inflate_stream_settings;

int ZEXPORT deflateInit_(z_streamp strm, int level, const char* version,
                         int stream_size) {
  Log(LogLevel::LOG_INFO, "deflateInit_ Line %d, strm %p, level %d\n", __LINE__,
      strm, level);

  deflate_stream_settings.Set(strm, level, Z_DEFLATED, 15, 8,
                              Z_DEFAULT_STRATEGY);
  return orig_deflateInit_(strm, level, version, stream_size);
}

int ZEXPORT deflateInit2_(z_streamp strm, int level, int method,
                          int window_bits, int mem_level, int strategy,
                          const char* version, int stream_size) {
  Log(LogLevel::LOG_INFO,
      "deflateInit2_ Line %d, strm %p, level %d, window_bits %d \n", __LINE__,
      strm, level, window_bits);

  deflate_stream_settings.Set(strm, level, method, window_bits, mem_level,
                              strategy);
  return orig_deflateInit2_(strm, level, method, window_bits, mem_level,
                            strategy, version, stream_size);
}

int ZEXPORT deflate(z_streamp strm, int flush) {
  DeflateSettings* deflate_settings = deflate_stream_settings.Get(strm);

  Log(LogLevel::LOG_INFO,
      "deflate Line %d, strm %p, avail_in %d, avail_out %d, flush %d, in_call "
      "%d, path %d\n",
      __LINE__, strm, strm->avail_in, strm->avail_out, flush, in_call,
      deflate_settings->path);

  int ret = 1;
  bool iaa_available = false;
  bool qat_available = false;
  if (!in_call && flush == Z_FINISH && deflate_settings->path != ZLIB) {
    uint32_t input_len = strm->avail_in;
    uint32_t output_len = strm->avail_out;

#ifdef USE_IAA
    iaa_available = config::use_iaa_compress &&
                    SupportedOptionsIAA(deflate_settings->window_bits,
                                        input_len, output_len);
#endif
#ifdef USE_QAT
    qat_available =
        config::use_qat_compress &&
        SupportedOptionsQAT(deflate_settings->window_bits, input_len);
#endif

    // If both accelerators are enabled, send configured ratio of requests to
    // one or the other
    ExecutionPath path_selected = ZLIB;
    if (iaa_available && qat_available) {
      if (std::rand() % 100 < config::iaa_compress_percentage) {
        path_selected = IAA;
      } else {
        path_selected = QAT;
      }
    } else if (iaa_available) {
      path_selected = IAA;
    } else if (qat_available) {
      path_selected = QAT;
    }

    if (path_selected == IAA) {
#ifdef USE_IAA
      in_call = true;
      ret = CompressIAA(strm->next_in, &input_len, strm->next_out, &output_len,
                        qpl_path_hardware, deflate_settings->window_bits);
      deflate_settings->path = IAA;
      in_call = false;
#endif  // USE_IAA
    } else if (path_selected == QAT) {
#ifdef USE_QAT
      in_call = true;
      ret = CompressQAT(strm->next_in, &input_len, strm->next_out, &output_len,
                        deflate_settings->window_bits);
      deflate_settings->path = QAT;
      in_call = false;
#endif  // USE_QAT
    }

    if (ret == 0) {
      strm->next_in += input_len;
      strm->avail_in -= input_len;
      strm->total_in += input_len;
      strm->next_out += output_len;
      strm->avail_out -= output_len;
      strm->total_out += output_len;
      if (strm->avail_in == 0) {
        ret = Z_STREAM_END;
      } else {
        ret = Z_BUF_ERROR;
      }

      Log(LogLevel::LOG_INFO,
          "deflate Line %d, strm %p, accelerator return code %d, avail_in %d, "
          "avail_out %d, path %d\n",
          __LINE__, strm, ret, strm->avail_in, strm->avail_out,
          deflate_settings->path);
      return ret;
    }
  }

  if (in_call || config::use_zlib_compress) {
    ret = orig_deflate(strm, flush);
    if (!in_call) {
      deflate_settings->path = ZLIB;
    }
  } else {
    ret = Z_DATA_ERROR;
  }

  Log(LogLevel::LOG_INFO,
      "deflate Line %d, strm %p, zlib return code %d, avail_in %d, "
      "avail_out %d, path %d\n",
      __LINE__, strm, ret, strm->avail_in, strm->avail_out,
      deflate_settings->path);

  return ret;
}

int ZEXPORT deflateEnd(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "deflateEnd Line %d, strm %p\n", __LINE__, strm);
  deflate_stream_settings.Unset(strm);
  return orig_deflateEnd(strm);
}

int ZEXPORT deflateReset(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "deflateReset Line %d, strm %p\n", __LINE__, strm);
  DeflateSettings* deflate_settings = deflate_stream_settings.Get(strm);
  if (deflate_settings != nullptr) {
    deflate_settings->path = UNDEFINED;
  }

  return orig_deflateReset(strm);
}

int ZEXPORT inflateInit_(z_streamp strm, const char* version, int stream_size) {
  inflate_stream_settings.Set(strm, 15);
  Log(LogLevel::LOG_INFO, "inflateInit_ Line %d, strm %p\n", __LINE__, strm);

  return orig_inflateInit_(strm, version, stream_size);
}

int ZEXPORT inflateInit2_(z_streamp strm, int window_bits, const char* version,
                          int stream_size) {
  inflate_stream_settings.Set(strm, window_bits);
  Log(LogLevel::LOG_INFO, "inflateInit2_ Line %d, strm %p, window_bits %d\n",
      __LINE__, strm, window_bits);

  return orig_inflateInit2_(strm, window_bits, version, stream_size);
}

int ZEXPORT inflate(z_streamp strm, int flush) {
  InflateSettings* inflate_settings = inflate_stream_settings.Get(strm);

  Log(LogLevel::LOG_INFO,
      "inflate Line %d, strm %p, avail_in %d, avail_out %d, flush %d, in_call "
      "%d, path %d\n",
      __LINE__, strm, strm->avail_in, strm->avail_out, flush, in_call,
      inflate_settings->path);
  PrintDeflateBlockHeader(LogLevel::LOG_INFO, strm->next_in, strm->avail_in,
                          inflate_settings->window_bits);

  int ret = 1;
  bool end_of_stream = true;
  bool iaa_available = false;
  bool qat_available = false;
  if (!in_call && strm->avail_in > 0 && inflate_settings->path != ZLIB) {
    uint32_t input_len = strm->avail_in;
    uint32_t output_len = strm->avail_out;

#ifdef USE_IAA
    iaa_available = config::use_iaa_uncompress &&
                    SupportedOptionsIAA(inflate_settings->window_bits,
                                        input_len, output_len) &&
                    IsIAADecompressible(strm->next_in, input_len,
                                        inflate_settings->window_bits);

#endif
#ifdef USE_QAT
    qat_available =
        config::use_qat_uncompress &&
        SupportedOptionsQAT(inflate_settings->window_bits, input_len);
#endif

    // If both accelerators are enabled, send configured ratio of requests to
    // one or the other
    ExecutionPath path_selected = ZLIB;
    if (iaa_available && qat_available) {
      if (std::rand() % 100 < config::iaa_uncompress_percentage) {
        path_selected = IAA;
      } else {
        path_selected = QAT;
      }
    } else if (iaa_available) {
      path_selected = IAA;
    } else if (qat_available) {
      path_selected = QAT;
    }

    if (path_selected == IAA) {
#ifdef USE_IAA
      in_call = true;
      ret = UncompressIAA(strm->next_in, &input_len, strm->next_out,
                          &output_len, qpl_path_hardware,
                          inflate_settings->window_bits, &end_of_stream);
      inflate_settings->path = IAA;
      in_call = false;
#endif  // USE_IAA
    } else if (path_selected == QAT) {
#ifdef USE_QAT
      in_call = true;
      ret =
          UncompressQAT(strm->next_in, &input_len, strm->next_out, &output_len,
                        inflate_settings->window_bits, &end_of_stream);
      inflate_settings->path = QAT;
      // QATzip does not support stateful decompression
      // Fall back to zlib if end-of-stream not reached in one call
      if (!end_of_stream) {
        ret = 1;
      }
      in_call = false;
#endif  // USE_QAT
    }

    if (ret == 0) {
      strm->next_in += input_len;
      strm->avail_in -= input_len;
      strm->total_in += input_len;
      strm->next_out += output_len;
      strm->avail_out -= output_len;
      strm->total_out += output_len;
      if (input_len > 0 || output_len > 0) {
        if (end_of_stream) {
          ret = Z_STREAM_END;
        } else {
          ret = Z_OK;
        }
      } else {
        ret = Z_BUF_ERROR;
      }

      Log(LogLevel::LOG_INFO,
          "inflate Line %d, strm %p, accelerator return code %d, avail_in %d, "
          "avail_out %d, end_of_stream %d, path %d\n",
          __LINE__, strm, ret, strm->avail_in, strm->avail_out, end_of_stream,
          inflate_settings->path);
      return ret;
    }
  }

  if (in_call || config::use_zlib_uncompress) {
    ret = orig_inflate(strm, flush);
    if (!in_call) {
      inflate_settings->path = ZLIB;
    }
  } else {
    ret = Z_DATA_ERROR;
  }

  Log(LogLevel::LOG_INFO,
      "inflate Line %d, strm %p, zlib return code %d, avail_in %d, avail_out "
      "%d, path %d\n",
      __LINE__, strm, ret, strm->avail_in, strm->avail_out,
      inflate_settings->path);

  return ret;
}

int ZEXPORT inflateEnd(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "inflateEnd Line %d, strm %p\n", __LINE__, strm);
  inflate_stream_settings.Unset(strm);
  return orig_inflateEnd(strm);
}

int ZEXPORT inflateReset(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "inflateReset Line %d, strm %p\n", __LINE__, strm);
  InflateSettings* inflate_settings = inflate_stream_settings.Get(strm);
  if (inflate_settings != nullptr) {
    inflate_settings->path = UNDEFINED;
  }

  return orig_inflateReset(strm);
}

int ZEXPORT compress2(Bytef* dest, uLongf* destLen, const Bytef* source,
                      uLong sourceLen, int level) {
  Log(LogLevel::LOG_INFO, "compress2 Line %d, sourceLen %lu, destLen %lu\n",
      __LINE__, sourceLen, *destLen);

  int ret = 1;
  uint32_t input_len = sourceLen;
  (void)input_len;
  uint32_t output_len = *destLen;

  bool iaa_available = false;
  bool qat_available = false;
#ifdef USE_IAA
  iaa_available = config::use_iaa_compress &&
                  SupportedOptionsIAA(15, input_len, output_len);
#endif
#ifdef USE_QAT
  qat_available =
      config::use_qat_compress && SupportedOptionsQAT(15, input_len);
#endif

  ExecutionPath path_selected = ZLIB;
  if (iaa_available) {
    path_selected = IAA;
  } else if (qat_available) {
    path_selected = QAT;
  }

  if (path_selected == IAA) {
#ifdef USE_IAA
    in_call = true;
    ret = CompressIAA(const_cast<uint8_t*>(source), &input_len, dest,
                      &output_len, qpl_path_hardware, 15);
    in_call = false;
#endif  // USE_IAA
  } else if (path_selected == QAT) {
#ifdef USE_QAT
    in_call = true;
    ret = CompressQAT(const_cast<uint8_t*>(source), &input_len, dest,
                      &output_len, 15);
    in_call = false;
#endif  // USE_QAT
  }

  if (ret == 0) {
    *destLen = output_len;
    ret = Z_OK;

    Log(LogLevel::LOG_INFO,
        "compress2 Line %d, accelerator return code %d, sourceLen %lu, "
        "destLen %lu\n",
        __LINE__, ret, sourceLen, *destLen);
  } else if (config::use_zlib_compress) {
    // compress2 in zlib calls deflate. It was observed that deflate is
    // sometimes intercepted by the shim. in_call prevents deflate from using
    // accelerators.
    in_call = true;
    ret = orig_compress2(dest, destLen, source, sourceLen, level);
    in_call = false;
    Log(LogLevel::LOG_INFO,
        "compress2 Line %d, zlib return code %d, sourceLen %lu, "
        "destLen %lu\n",
        __LINE__, ret, sourceLen, *destLen);
  } else {
    ret = Z_DATA_ERROR;
  }
  return ret;
}

int ZEXPORT compress(Bytef* dest, uLongf* destLen, const Bytef* source,
                     uLong sourceLen) {
  return compress2(dest, destLen, source, sourceLen, Z_DEFAULT_COMPRESSION);
}

int ZEXPORT uncompress2(Bytef* dest, uLongf* destLen, const Bytef* source,
                        uLong* sourceLen) {
  Log(LogLevel::LOG_INFO, "uncompress2 Line %d, sourceLen %lu, destLen %lu\n",
      __LINE__, *sourceLen, *destLen);

  int ret = 1;
  bool end_of_stream = true;
  (void)end_of_stream;
  uint32_t input_len = *sourceLen;
  uint32_t output_len = *destLen;

  bool iaa_available = false;
  bool qat_available = false;
#ifdef USE_IAA
  iaa_available =
      config::use_iaa_uncompress &&
      SupportedOptionsIAA(15, input_len, output_len) &&
      IsIAADecompressible(const_cast<uint8_t*>(source), input_len, 15);
#endif
#ifdef USE_QAT
  qat_available =
      config::use_qat_uncompress && SupportedOptionsQAT(15, input_len);
#endif

  ExecutionPath path_selected = ZLIB;
  if (iaa_available) {
    path_selected = IAA;
  } else if (qat_available) {
    path_selected = QAT;
  }

  if (path_selected == IAA) {
#ifdef USE_IAA
    in_call = true;
    ret = UncompressIAA(const_cast<uint8_t*>(source), &input_len, dest,
                        &output_len, qpl_path_hardware, 15, &end_of_stream);
    in_call = false;
#endif  // USE_IAA
  } else if (path_selected == QAT) {
#ifdef USE_QAT
    in_call = true;
    ret = UncompressQAT(const_cast<uint8_t*>(source), &input_len, dest,
                        &output_len, 15, &end_of_stream);
    in_call = false;
#endif  // USE_QAT
  }

  if (ret == 0) {
    *sourceLen = input_len;
    *destLen = output_len;
    ret = Z_OK;

    Log(LogLevel::LOG_INFO,
        "uncompress2 Line %d, accelerator return code %d, sourceLen %lu, "
        "destLen %lu\n",
        __LINE__, ret, *sourceLen, *destLen);
  } else if (config::use_zlib_uncompress) {
    // refer to comment in compress2
    in_call = true;
    ret = orig_uncompress2(dest, destLen, source, sourceLen);
    in_call = false;
    Log(LogLevel::LOG_INFO,
        "uncompress2 Line %d, zlib return code %d, sourceLen %lu, "
        "destLen %lu\n",
        __LINE__, ret, *sourceLen, *destLen);
  } else {
    ret = Z_DATA_ERROR;
  }
  return ret;
}

int ZEXPORT uncompress(Bytef* dest, uLongf* destLen, const Bytef* source,
                       uLong sourceLen) {
  uLong srcLen = sourceLen;
  return uncompress2(dest, destLen, source, &srcLen);
}

ExecutionPath GetDeflateExecutionPath(z_streamp strm) {
  DeflateSettings* deflate_settings = deflate_stream_settings.Get(strm);
  return deflate_settings->path;
}

ExecutionPath GetInflateExecutionPath(z_streamp strm) {
  InflateSettings* inflate_settings = inflate_stream_settings.Get(strm);
  return inflate_settings->path;
}

enum class FileMode { NONE, READ, WRITE, APPEND };

struct GzipFile {
  GzipFile() { Reset(); }

  GzipFile(int _fd, FileMode file_mode) : fd(_fd), mode(file_mode) { Reset(); }

  ~GzipFile() {
    if (data_buf != nullptr) {
      delete[] data_buf;
    }
    if (io_buf != nullptr) {
      delete[] io_buf;
    }
    orig_deflateEnd(&deflate_stream);
    orig_inflateEnd(&inflate_stream);
  }

  void Reset() {
    path = UNDEFINED;
    use_zlib_for_decompression = false;
    reached_eof = false;

    data_buf_pos = 0;
    data_buf_content = 0;
    io_buf_pos = 0;
    io_buf_content = 0;

    memset(&deflate_stream, 0, sizeof(z_stream));
    orig_deflateInit2_(&deflate_stream, -1, Z_DEFLATED, 31, 8,
                       Z_DEFAULT_STRATEGY, ZLIB_VERSION, (int)sizeof(z_stream));
    memset(&inflate_stream, 0, sizeof(z_stream));
    orig_inflateInit2_(&inflate_stream, 31, ZLIB_VERSION,
                       (int)sizeof(z_stream));
  }

  void AllocateBuffers() {
    if (data_buf == nullptr) {
      data_buf = new char[alloc_size];
      data_buf_pos = 0;
      data_buf_content = 0;
    }
    if (io_buf == nullptr) {
      io_buf = new char[alloc_size];
      io_buf_pos = 0;
      io_buf_content = 0;
    }
  }

  int fd = 0;
  ExecutionPath path = UNDEFINED;
  // If falling back to zlib at some point, all data from there forward must be
  // decompressed with zlib
  bool use_zlib_for_decompression = false;
  bool reached_eof = false;
  FileMode mode = FileMode::NONE;

  // For gzwrite
  // data_buf --(compress)--> io_buf --(write)--> file
  // - buffer input data into data_buf
  // - once size reached, compress the data into io_buf
  // - write io_buf to file

  // For gzread
  // file --(read)--> io_buf --(uncompress)--> data_buf
  // - read file data into io_buf
  // - decompress data into data_buf
  // - serve data from data_buf when requested

  char* data_buf = nullptr;
  int data_buf_size = 0;
  int data_buf_pos = 0;
  int data_buf_content = 0;

  char* io_buf = nullptr;
  int io_buf_size = 0;
  int io_buf_pos = 0;
  int io_buf_content = 0;

  const int alloc_size = 512 << 10;

  // Stream to use zlib in case of accelerator errors
  z_stream deflate_stream;
  z_stream inflate_stream;
};

class GzipFiles {
 public:
  void Set(gzFile file, int fd, FileMode file_mode) {
    GzipFile* f = new GzipFile(fd, file_mode);
    map.Set(file, f);
  }

  void Unset(gzFile file) { map.Unset(file); }

  GzipFile* Get(gzFile file) { return map.Get(file); }

 private:
  ShardedMap<gzFile, GzipFile*> map;
};
GzipFiles gzip_files;

// Inspired by gz_open in gzlib.c
int GetOpenFlags(const char* mode, FileMode* file_mode) {
  bool cloexec = false;
  bool exclusive = false;

  while (*mode) {
    // TODO not all modes covered. Verify if any more to add.
    switch (*mode) {
      case 'r':
        *file_mode = FileMode::READ;
        break;
      case 'w':
        *file_mode = FileMode::WRITE;
        break;
      case 'a':
        *file_mode = FileMode::APPEND;
        break;
      case 'b':
        break;
#ifdef O_CLOEXEC
      case 'e':
        cloexec = true;
        break;
#endif
#ifdef O_EXCL
      case 'x':
        exclusive = true;
        break;
#endif
      default:;
    }
    mode++;
  }

  /* compute the flags for open() */
  int oflag = 0;
  oflag =
#ifdef O_LARGEFILE
      O_LARGEFILE |
#endif
#ifdef O_BINARY
      O_BINARY |
#endif
#ifdef O_CLOEXEC
      (cloexec ? O_CLOEXEC : 0) |
#endif
      (*file_mode == FileMode::READ
           ? O_RDONLY
           : (O_WRONLY | O_CREAT |
#ifdef O_EXCL
              (exclusive ? O_EXCL : 0) |
#endif
              (*file_mode == FileMode::WRITE ? O_TRUNC : O_APPEND)));

  return oflag;
}

gzFile ZEXPORT gzopen(const char* path, const char* mode) {
  // We need to store the file descriptor for use in other functions.
  // Open the file here and then call gzdopen
  FileMode file_mode = FileMode::NONE;
  int oflag = GetOpenFlags(mode, &file_mode);
  int fd = open((const char*)path, oflag, 0666);
  gzFile file = orig_gzdopen(fd, mode);
  // TODO in case of error fall back to zlib and set execution path.

  Log(LogLevel::LOG_INFO, "gzopen Line %d, file %p, path %s, mode %s\n",
      __LINE__, file, path, mode);

  gzip_files.Set(file, fd, file_mode);
  return file;
}

gzFile ZEXPORT gzdopen(int fd, const char* mode) {
  gzFile file = orig_gzdopen(fd, mode);

  Log(LogLevel::LOG_INFO, "gzdopen Line %d, file %d, fd %p, mode %s\n",
      __LINE__, fd, file, mode);

  FileMode file_mode = FileMode::NONE;
  GetOpenFlags(mode, &file_mode);

  gzip_files.Set(file, fd, file_mode);
  return file;
}

static int GzwriteAcceleratorCompress(GzipFile* gz, uint8_t* input,
                                      uint32_t* input_length, uint8_t* output,
                                      uint32_t* output_length) {
  (void)gz;
  (void)input;
  (void)input_length;
  (void)output;
  (void)output_length;

  int ret = 1;
  bool iaa_available = false;
  bool qat_available = false;

#ifdef USE_IAA
  iaa_available = config::use_iaa_compress &&
                  SupportedOptionsIAA(31, *input_length, *output_length);
#endif
#ifdef USE_QAT
  qat_available =
      config::use_qat_compress && SupportedOptionsQAT(31, *input_length);
#endif

  ExecutionPath path_selected = ZLIB;
  if (qat_available) {
    path_selected = QAT;
  } else if (iaa_available) {
    path_selected = IAA;
  }

  if (path_selected == IAA) {
#ifdef USE_IAA
    in_call = true;
    ret = CompressIAA(input, input_length, output, output_length,
                      qpl_path_hardware, 31, true);
    gz->path = IAA;
    in_call = false;
#endif  // USE_IAA
  } else if (path_selected == QAT) {
#ifdef USE_QAT
    in_call = true;
    ret = CompressQAT(input, input_length, output, output_length, 31, true);
    gz->path = QAT;
    in_call = false;
#endif  // USE_QAT
  }
  return ret;
}

static int GzreadAcceleratorUncompress(GzipFile* gz, uint8_t* input,
                                       uint32_t* input_length, uint8_t* output,
                                       uint32_t* output_length,
                                       bool* end_of_stream) {
  (void)gz;
  (void)input;
  (void)input_length;
  (void)output;
  (void)output_length;
  (void)end_of_stream;

  int ret = 1;
  bool iaa_available = false;
  bool qat_available = false;

#ifdef USE_IAA
  iaa_available = config::use_iaa_uncompress &&
                  SupportedOptionsIAA(31, *input_length, *output_length) &&
                  IsIAADecompressible(input, *input_length, 31);
#endif
#ifdef USE_QAT
  qat_available =
      config::use_qat_uncompress && SupportedOptionsQAT(31, *input_length);
#endif

  ExecutionPath path_selected = ZLIB;
  if (qat_available) {
    path_selected = QAT;
  } else if (iaa_available) {
    path_selected = IAA;
  }

  if (path_selected == IAA) {
#ifdef USE_IAA
    in_call = true;
    ret = UncompressIAA(input, input_length, output, output_length,
                        qpl_path_hardware, 31, end_of_stream, true);
    gz->path = IAA;
    in_call = false;
#endif  // USE_IAA
  } else if (path_selected == QAT) {
#ifdef USE_QAT
    in_call = true;
    ret = UncompressQAT(input, input_length, output, output_length, 31,
                        end_of_stream, true);
    gz->path = QAT;
    in_call = false;
#endif  // USE_QAT
  }
  return ret;
}

static int GzwriteZlibCompress(gzFile file, voidpc buf, unsigned len) {
  int ret = 0;
  if (config::use_zlib_compress) {
    ret = orig_gzwrite(file, buf, len);
  } else {
    ret = 0;
  }
  return ret;
}

static int GzreadZlibUncompress(gzFile file, voidp buf, unsigned len) {
  int ret = 0;
  if (config::use_zlib_uncompress) {
    ret = orig_gzread(file, buf, len);
  } else {
    ret = -1;
  }
  return ret;
}

static int CompressAndWrite(gzFile file, GzipFile* gz) {
  (void)file;
  uint32_t input_len = gz->data_buf_content;
  uint8_t* input = reinterpret_cast<uint8_t*>(gz->data_buf);
  uint32_t output_len = gz->io_buf_size;
  uint8_t* output = reinterpret_cast<uint8_t*>(gz->io_buf);
  // TODO loop in case not all data compressed
  int ret =
      GzwriteAcceleratorCompress(gz, input, &input_len, output, &output_len);
  Log(LogLevel::LOG_INFO,
      "CompressAndWrite Line %d, file %p, accelerator return code %d, input "
      "%d, "
      "output %d\n",
      __LINE__, file, ret, input_len, output_len);

  if (ret == 0) {
    gz->data_buf_pos = input_len;
  } else {
    gz->deflate_stream.next_in = (Bytef*)(gz->data_buf);
    gz->deflate_stream.avail_in =
        static_cast<unsigned int>(gz->data_buf_content);
    gz->deflate_stream.next_out = (Bytef*)(gz->io_buf);
    gz->deflate_stream.avail_out = static_cast<unsigned int>(gz->io_buf_size);
    ret = orig_deflate(&gz->deflate_stream, Z_FINISH);
    Log(LogLevel::LOG_INFO,
        "CompressAndWrite Line %d, file %p, zlib return code %d, input %d, "
        "output %d, avail_in %d, avail_out %d\n",
        __LINE__, file, ret, input_len, output_len, gz->deflate_stream.avail_in,
        gz->deflate_stream.avail_out);
    if (ret == Z_STREAM_END) {
      gz->data_buf_pos = gz->data_buf_content - gz->deflate_stream.avail_in;
      output_len = gz->io_buf_size - gz->deflate_stream.avail_out;
      orig_deflateReset(&gz->deflate_stream);
    } else {
      return 1;
    }
  }

  int write_ret = 0;
  do {
    write_ret = write(gz->fd, gz->io_buf, output_len);
    Log(LogLevel::LOG_INFO,
        "CompressAndWrite Line %d, file %p, written to file %d\n", __LINE__,
        file, write_ret);
    if (write_ret >= 0) {
      output_len -= write_ret;
    }
  } while (output_len > 0 && write_ret >= 0);

  if (write_ret == -1) {
    return 1;
  }

  return 0;
}

int ZEXPORT gzwrite(gzFile file, voidpc buf, unsigned len) {
  GzipFile* gz = gzip_files.Get(file);
  Log(LogLevel::LOG_INFO, "gzwrite Line %d, file %p, buf %p, len %u\n",
      __LINE__, file, buf, len);

  unsigned int written_bytes = 0;
  bool accelerator_selected =
      config::use_iaa_compress || config::use_qat_compress;
  if (gz->path != ZLIB && accelerator_selected) {
    gz->AllocateBuffers();
    gz->data_buf_size = 256 << 10;
    gz->io_buf_size = 512 << 10;

    while (written_bytes < len) {
      // If buffer not full, add data to buffer, else write to file
      uint32_t len_to_write = len - written_bytes;
      uint32_t data_buf_remaining = gz->data_buf_size - gz->data_buf_content;
      uint32_t data_to_copy = data_buf_remaining >= len_to_write
                                  ? len_to_write
                                  : data_buf_remaining;
      memcpy(gz->data_buf + gz->data_buf_content, (char*)buf + written_bytes,
             data_to_copy);
      gz->data_buf_content += data_to_copy;
      written_bytes += data_to_copy;
      Log(LogLevel::LOG_INFO,
          "gzwrite Line %d, file %p, remaining %u, to copy %u, written %u\n",
          __LINE__, file, data_buf_remaining, data_to_copy, written_bytes);

      // Compress and write the buffer
      if (written_bytes < len) {
        int ret = CompressAndWrite(file, gz);
        if (ret != 0) {
          written_bytes = 0;
          goto gzwrite_end;
        }

        // Shift any remaining content of data_buf to beginning
        // TODO replace with circular buffer to avoid copy
        uint32_t data_remaining = gz->data_buf_content - gz->data_buf_pos;
        memmove(gz->data_buf, gz->data_buf + gz->data_buf_pos, data_remaining);
        gz->data_buf_content = data_remaining;
        gz->data_buf_pos = 0;
      }
    }
  } else {
    written_bytes = GzwriteZlibCompress(file, buf, len);
    gz->path = ZLIB;
  }

gzwrite_end:
  Log(LogLevel::LOG_INFO,
      "gzwrite Line %d, file %p, "
      "written %d, buffered %d, path %d\n",
      __LINE__, file, written_bytes, gz->data_buf_pos, gz->path);

  return written_bytes;
}

int ZEXPORT gzread(gzFile file, voidp buf, unsigned len) {
  GzipFile* gz = gzip_files.Get(file);

  Log(LogLevel::LOG_INFO, "gzread Line %d, file %p, buf %p, len %u\n", __LINE__,
      file, buf, len);

  int ret = 1;
  uint32_t read_bytes = 0;
  bool accelerator_selected =
      config::use_iaa_uncompress || config::use_qat_uncompress;
  if (gz->path != ZLIB && accelerator_selected) {
    gz->AllocateBuffers();
    gz->data_buf_size = 512 << 10;
    gz->io_buf_size = 512 << 10;

    bool more_data = true;
    while (read_bytes < len && more_data) {
      // Get uncompressed data from data_buf
      uint32_t len_to_read = len - read_bytes;
      uint32_t data_remaining = gz->data_buf_content - gz->data_buf_pos;
      uint32_t data_to_copy =
          data_remaining >= len_to_read ? len_to_read : data_remaining;
      memcpy((char*)buf + read_bytes, gz->data_buf + gz->data_buf_pos,
             data_to_copy);
      gz->data_buf_pos += data_to_copy;
      read_bytes += data_to_copy;
      Log(LogLevel::LOG_INFO,
          "gzread Line %d, file %p, remaining %u, to copy %u, read %u\n",
          __LINE__, file, data_remaining, data_to_copy, read_bytes);

      // If not enough uncompressed data in data_buf, read and decompress more
      // (if more available)
      if (read_bytes < len) {
        uint32_t io_buf_remaining = gz->io_buf_content - gz->io_buf_pos;
        bool file_data_remaining = !gz->reached_eof || (io_buf_remaining > 0);
        if (file_data_remaining) {
          // data_buf is now empty
          gz->data_buf_content = 0;
          gz->data_buf_pos = 0;

          // Read from file into compressed data buffer io_buf.
          // Append new data to any existing data already in the buffer.
          int read_ret = 0;
          do {
            read_ret = read(gz->fd, gz->io_buf + gz->io_buf_content,
                            gz->io_buf_size - gz->io_buf_content);
            if (read_ret > 0) {
              gz->io_buf_content += read_ret;
            }
            Log(LogLevel::LOG_INFO,
                "gzread Line %d, file %p, read from file %d\n", __LINE__, file,
                read_ret);
          } while (gz->io_buf_content < gz->io_buf_size && read_ret > 0);

          // Check for EOF/error
          if (read_ret == 0) {
            gz->reached_eof = true;
          } else if (read_ret < 0) {
            // If there is an error reading from file, calling orig_gzread
            // probably won't work either
            // TODO if this is the first call to gzread we could try to call
            // orig_gzread
            read_bytes = -1;
            goto gzread_end;
          }

          // Decompress content of io_buf into data_buf
          uint32_t input_len = gz->io_buf_content;
          uint8_t* input = reinterpret_cast<uint8_t*>(gz->io_buf);
          uint32_t output_len = gz->data_buf_size;
          uint8_t* output = reinterpret_cast<uint8_t*>(gz->data_buf);
          if (!gz->use_zlib_for_decompression) {
            bool end_of_stream = false;
            ret = GzreadAcceleratorUncompress(gz, input, &input_len, output,
                                              &output_len, &end_of_stream);
            Log(LogLevel::LOG_INFO,
                "gzread Line %d, file %p, accelerator return code %d, input "
                "%d, "
                "output %d\n",
                __LINE__, file, ret, input_len, output_len);

            // If we didn't reach end-of-stream, it means io_buf is not large
            // enough to hold the entire stream
            if (ret != 0 || !end_of_stream) {
              // Once switching to zlib, never go back to accelerators
              // The input may contain large streams that zlib will handle over
              // multiple calls
              gz->use_zlib_for_decompression = true;
            } else {
              gz->io_buf_pos += input_len;
              gz->data_buf_content += output_len;
            }
          }

          if (gz->use_zlib_for_decompression) {
            gz->inflate_stream.next_in = (Bytef*)(gz->io_buf);
            gz->inflate_stream.avail_in =
                static_cast<unsigned int>(gz->io_buf_content);
            gz->inflate_stream.next_out = (Bytef*)(gz->data_buf);
            gz->inflate_stream.avail_out =
                static_cast<unsigned int>(gz->data_buf_size);
            ret = orig_inflate(&gz->inflate_stream, Z_SYNC_FLUSH);
            Log(LogLevel::LOG_INFO,
                "gzread Line %d, file %p, zlib return code %d, input %d, "
                "output %d, avail_in %d, avail_out %d\n",
                __LINE__, file, ret, input_len, output_len,
                gz->inflate_stream.avail_in, gz->inflate_stream.avail_out);
            if (ret == Z_STREAM_END || ret == Z_OK) {
              gz->io_buf_pos +=
                  (gz->io_buf_content - gz->inflate_stream.avail_in);
              gz->data_buf_content +=
                  (gz->data_buf_size - gz->inflate_stream.avail_out);
              if (ret == Z_STREAM_END) {
                orig_inflateReset(&gz->inflate_stream);
              }
            } else if (ret != Z_OK) {
              read_bytes = -1;
              goto gzread_end;
            }
          }

          // Shift any remaining content of io_buf to beginning
          // TODO replace with circular buffer to avoid copy
          io_buf_remaining = gz->io_buf_content - gz->io_buf_pos;
          memmove(gz->io_buf, gz->io_buf + gz->io_buf_pos, io_buf_remaining);
          gz->io_buf_content = io_buf_remaining;
          gz->io_buf_pos = 0;
        } else {
          more_data = false;
        }
      }
    }
  } else {
    read_bytes = GzreadZlibUncompress(file, buf, len);
    gz->path = ZLIB;
  }

gzread_end:
  Log(LogLevel::LOG_INFO,
      "gzread Line %d, file %p, return code %d, "
      "read %d, buffered compressed %d, buffered uncompressed %d, path %d\n",
      __LINE__, file, ret, read_bytes, gz->io_buf_content,
      gz->data_buf_content - gz->data_buf_pos, gz->path);
  return read_bytes;
}

int ZEXPORT gzclose(gzFile file) {
  GzipFile* gz = gzip_files.Get(file);

  Log(LogLevel::LOG_INFO, "gzclose Line %d, file %p, buffered %d, path %d\n",
      __LINE__, file, gz->data_buf_content, gz->path);

  int ret = 0;
  if (gz->path != ZLIB &&
      (gz->mode == FileMode::WRITE || gz->mode == FileMode::APPEND)) {
    // Compress any remaining buffered data
    int write_ret = 0;
    if (gz->data_buf_content > 0) {
      write_ret = CompressAndWrite(file, gz);
    }

    // Capture file size and name before gzclose
    int file_size = lseek(gz->fd, 0, SEEK_CUR);
    char file_path[MAXPATHLEN];
    ssize_t readlink_ret =
        readlink(("/proc/self/fd/" + std::to_string(gz->fd)).c_str(), file_path,
                 MAXPATHLEN - 1);
    if (readlink_ret == -1) {
      ret = orig_gzclose(file);
      gzip_files.Unset(file);
      Log(LogLevel::LOG_ERROR, "gzclose Line %d, readlink_ret return error \n",
          __LINE__);
      return ret;
    }
    file_path[readlink_ret] = '\0';

    int close_ret = orig_gzclose(file);

    // Remove any file content added by gzclose
    int truncate_ret = 0;
    if (file_size != -1) {
      truncate_ret = truncate(file_path, file_size);
    }

    if (write_ret != 0) {
      ret = Z_STREAM_ERROR;
    } else if (close_ret != Z_OK) {
      ret = close_ret;
    } else if (truncate_ret != 0) {
      ret = Z_STREAM_ERROR;
    }
  } else {
    ret = orig_gzclose(file);
  }

  Log(LogLevel::LOG_INFO,
      "gzclose Line %d, file %p, return code %d, buffered processed %d\n",
      __LINE__, file, ret, gz->data_buf_pos);
  gzip_files.Unset(file);
  return ret;
}

int ZEXPORT gzeof(gzFile file) {
  GzipFile* gz = gzip_files.Get(file);
  return gz->reached_eof;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
