// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "zlib_accel.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/param.h>
#include <unistd.h>

#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "config/config.h"
#include "logging.h"
#include "sharded_map.h"
#ifdef USE_IAA
#include "iaa.h"
#endif
#ifdef USE_IGZIP
#include "igzip.h"
#endif
#ifdef USE_QAT
#include "qat.h"
#endif
#include "statistics.h"

using namespace config;

// Window bits: 15 = zlib (deflate) format, 31 = gzip format
static constexpr int kWindowBitsZlib = 15;
static constexpr int kWindowBitsGzip = 31;

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
static int (*orig_deflateSetDictionary)(z_streamp strm, const Bytef* dictionary,
                                        uInt dictLength);
static int (*orig_deflate)(z_streamp strm, int flush);
static int (*orig_deflateEnd)(z_streamp strm);
static int (*orig_deflateReset)(z_streamp strm);
static int (*orig_deflateResetKeep)(z_streamp strm);
static int (*orig_deflateParams)(z_streamp strm, int level, int strategy);
static int (*orig_deflateCopy)(z_streamp dest, z_streamp source);
static int (*orig_inflateInit_)(z_streamp strm, const char* version,
                                int stream_size);
static int (*orig_inflateInit2_)(z_streamp strm, int window_bits,
                                 const char* version, int stream_size);
static int (*orig_inflateSetDictionary)(z_streamp strm, const Bytef* dictionary,
                                        uInt dictLength);
static int (*orig_inflate)(z_streamp strm, int flush);
static int (*orig_inflateEnd)(z_streamp strm);
static int (*orig_inflateReset)(z_streamp strm);
static int (*orig_inflateResetKeep)(z_streamp strm);
static int (*orig_inflateReset2)(z_streamp strm, int windowBits);
static int (*orig_inflateCopy)(z_streamp dest, z_streamp source);
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
static int (*orig_gzclose_r)(gzFile file);
static int (*orig_gzclose_w)(gzFile file);
static int (*orig_gzeof)(gzFile file);
static int (*orig_gzsetparams)(gzFile file, int level, int strategy);
static int (*orig_gzflush)(gzFile file, int flush);
static int (*orig_gzputc)(gzFile file, int c);
static int (*orig_gzputs)(gzFile file, const char* s);
static z_size_t (*orig_gzfwrite)(voidpc buf, z_size_t size, z_size_t nitems,
                                 gzFile file);
static int (*orig_gzvprintf)(gzFile file, const char* format, va_list va);
static int (*orig_gzgetc)(gzFile file);
static int (*orig_gzungetc)(int c, gzFile file);
static char* (*orig_gzgets)(gzFile file, char* buf, int len);
static z_size_t (*orig_gzfread)(voidp buf, z_size_t size, z_size_t nitems,
                                gzFile file);

// Forward declaration — defined after DeflateStreamSettings,
// InflateStreamSettings, and GzipFiles class definitions below
static void InitStreamRegistries();

// Initialize/cleanup functions when library is loaded
static int init_zlib_accel(void) __attribute__((constructor));
static void cleanup_zlib_accel(void) __attribute__((destructor));

// Number of orig_* symbols that failed to resolve. Only used for the summary
// log below: each wrapper checks the specific pointer it needs, so one missing
// symbol degrades only the paths that actually call it.
static int missing_symbol_count = 0;

// Macro that loads symbols. A failure is logged but does not stop the sequence:
// returning early from the constructor would leave every later orig_* pointer
// null (and the return value of a constructor is ignored anyway), so keep going
// and resolve as many as possible.
#define LOAD_SYMBOL(fptr, type, name)                                          \
  do {                                                                         \
    dlerror();                                                                 \
    fptr = reinterpret_cast<type>(dlsym(RTLD_NEXT, name));                     \
    const char* error = dlerror();                                             \
    if (error != nullptr) {                                                    \
      Log(LogLevel::LOG_ERROR, "init_zlib_accel Line ", __LINE__,              \
          "Failed to load symbol '", name, "': ", error, "\n");                \
      fptr = nullptr;                                                          \
      missing_symbol_count++;                                                  \
    } else if (fptr == nullptr) {                                              \
      Log(LogLevel::LOG_ERROR, "init_zlib_accel Line ", __LINE__, " Symbol '", \
          name, "' resolved to NULL\n");                                       \
      missing_symbol_count++;                                                  \
    }                                                                          \
  } while (0)

static int init_zlib_accel(void) {
  // Load deflate functions
  LOAD_SYMBOL(orig_deflateInit_, int (*)(z_streamp, int, const char*, int),
              "deflateInit_");

  LOAD_SYMBOL(orig_deflateInit2_,
              int (*)(z_streamp, int, int, int, int, int, const char*, int),
              "deflateInit2_");

  LOAD_SYMBOL(orig_deflateSetDictionary, int (*)(z_streamp, const Bytef*, uInt),
              "deflateSetDictionary");

  LOAD_SYMBOL(orig_deflate, int (*)(z_streamp, int), "deflate");

  LOAD_SYMBOL(orig_deflateEnd, int (*)(z_streamp), "deflateEnd");

  LOAD_SYMBOL(orig_deflateReset, int (*)(z_streamp), "deflateReset");

  LOAD_SYMBOL(orig_deflateResetKeep, int (*)(z_streamp), "deflateResetKeep");

  LOAD_SYMBOL(orig_deflateParams, int (*)(z_streamp, int, int),
              "deflateParams");

  LOAD_SYMBOL(orig_deflateCopy, int (*)(z_streamp, z_streamp), "deflateCopy");

  // Load inflate functions
  LOAD_SYMBOL(orig_inflateInit_, int (*)(z_streamp, const char*, int),
              "inflateInit_");

  LOAD_SYMBOL(orig_inflateInit2_, int (*)(z_streamp, int, const char*, int),
              "inflateInit2_");

  LOAD_SYMBOL(orig_inflateSetDictionary, int (*)(z_streamp, const Bytef*, uInt),
              "inflateSetDictionary");

  LOAD_SYMBOL(orig_inflate, int (*)(z_streamp, int), "inflate");

  LOAD_SYMBOL(orig_inflateEnd, int (*)(z_streamp), "inflateEnd");

  LOAD_SYMBOL(orig_inflateReset, int (*)(z_streamp), "inflateReset");

  LOAD_SYMBOL(orig_inflateResetKeep, int (*)(z_streamp), "inflateResetKeep");

  LOAD_SYMBOL(orig_inflateReset2, int (*)(z_streamp, int), "inflateReset2");

  LOAD_SYMBOL(orig_inflateCopy, int (*)(z_streamp, z_streamp), "inflateCopy");

  // Load compress/uncompress functions
  LOAD_SYMBOL(orig_compress, int (*)(Bytef*, uLongf*, const Bytef*, uLong),
              "compress");

  LOAD_SYMBOL(orig_compress2,
              int (*)(Bytef*, uLongf*, const Bytef*, uLong, int), "compress2");

  LOAD_SYMBOL(orig_uncompress, int (*)(Bytef*, uLongf*, const Bytef*, uLong),
              "uncompress");

  LOAD_SYMBOL(orig_uncompress2, int (*)(Bytef*, uLongf*, const Bytef*, uLong*),
              "uncompress2");

  // Load gzip functions
  LOAD_SYMBOL(orig_gzopen, gzFile(*)(const char*, const char*), "gzopen");

  LOAD_SYMBOL(orig_gzdopen, gzFile(*)(int, const char*), "gzdopen");

  LOAD_SYMBOL(orig_gzwrite, int (*)(gzFile, voidpc, unsigned), "gzwrite");

  LOAD_SYMBOL(orig_gzread, int (*)(gzFile, voidp, unsigned), "gzread");

  LOAD_SYMBOL(orig_gzclose, int (*)(gzFile), "gzclose");

  LOAD_SYMBOL(orig_gzclose_r, int (*)(gzFile), "gzclose_r");

  LOAD_SYMBOL(orig_gzclose_w, int (*)(gzFile), "gzclose_w");

  LOAD_SYMBOL(orig_gzeof, int (*)(gzFile), "gzeof");

  LOAD_SYMBOL(orig_gzsetparams, int (*)(gzFile, int, int), "gzsetparams");

  LOAD_SYMBOL(orig_gzflush, int (*)(gzFile, int), "gzflush");

  LOAD_SYMBOL(orig_gzputc, int (*)(gzFile, int), "gzputc");

  LOAD_SYMBOL(orig_gzputs, int (*)(gzFile, const char*), "gzputs");

  LOAD_SYMBOL(orig_gzfwrite, z_size_t(*)(voidpc, z_size_t, z_size_t, gzFile),
              "gzfwrite");

  LOAD_SYMBOL(orig_gzvprintf, int (*)(gzFile, const char*, va_list),
              "gzvprintf");

  LOAD_SYMBOL(orig_gzgetc, int (*)(gzFile), "gzgetc");

  LOAD_SYMBOL(orig_gzungetc, int (*)(int, gzFile), "gzungetc");

  LOAD_SYMBOL(orig_gzgets, char* (*)(gzFile, char*, int), "gzgets");

  LOAD_SYMBOL(orig_gzfread, z_size_t(*)(voidp, z_size_t, z_size_t, gzFile),
              "gzfread");

  if (missing_symbol_count > 0) {
    Log(LogLevel::LOG_ERROR, "init_zlib_accel Line ", __LINE__, " ",
        missing_symbol_count,
        " zlib symbol(s) could not be resolved; the affected entry points fall "
        "back to zlib where possible and return Z_VERSION_ERROR where not\n");
  }

  // Load configuration file; on failure (file absent or is a symlink) continue
  // with compiled-in defaults — a missing config is not fatal.
  std::string config_file_content;
  const bool config_loaded = config::LoadConfigFile(config_file_content);
  if (!config_loaded) {
    Log(LogLevel::LOG_ERROR,
        "Failed to load configuration file, continuing with defaults\n");
  }

  InitStreamRegistries();

#if defined(DEBUG_LOG) || defined(ENABLE_STATISTICS)
  if (config_loaded && !config::log_file.empty()) {
    CreateLogFile(config::log_file.c_str());
  }
#endif

  return 0;
}

static void cleanup_zlib_accel(void) {
#if defined(DEBUG_LOG) || defined(ENABLE_STATISTICS)
  CloseLogFile();
#endif
}

#undef LOAD_SYMBOL

// Avoid recursive call (e.g., if QATzip falls back to zlib internally)
static thread_local bool in_call = false;

constexpr uint8_t ZLIB_FDICT_MASK = 0x20;

#ifdef DEBUG_LOG
// Readable name for the numeric path in log output. Only referenced from Log(),
// which compiles away entirely when DEBUG_LOG is off.
static const char* ExecutionPathName(ExecutionPath path) {
  switch (path) {
    case UNDEFINED:
      return "UNDEFINED";
    case ZLIB:
      return "ZLIB";
    case QAT:
      return "QAT";
    case IAA:
      return "IAA";
    case IGZIP:
      return "IGZIP";
  }
  return "UNKNOWN";
}
#endif  // DEBUG_LOG

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
  struct isal_zstream* isal_strm = nullptr;
  // Set once the shim has reported Z_STREAM_END for this stream. Only an
  // offloaded completion sets it: an accelerator finishes the stream without
  // ever feeding zlib's own state, so zlib cannot report the terminal state
  // afterwards and every later call would be dispatched from scratch. A
  // ZLIB-path stream is left alone, since zlib tracks this itself.
  bool stream_end_reached = false;
};

struct InflateSettings {
  InflateSettings(int _window_bits) : window_bits(_window_bits) {}
  int window_bits;
  ExecutionPath path = UNDEFINED;
  struct inflate_state* isal_strm = nullptr;
  // See DeflateSettings::stream_end_reached.
  bool stream_end_reached = false;
};

// isal_strm is a raw pointer, so destroying a settings object does not free the
// ISA-L stream it owns. Every path that discards an entry has to come through
// here: *End() when the caller is done with the stream, and Set()/SetFromCopy()
// when a new entry replaces one that is still holding state.
static void ReleaseDeflateIgzipState(
    const std::shared_ptr<DeflateSettings>& settings) {
  if (settings == nullptr || settings->isal_strm == nullptr) {
    return;
  }
#ifdef USE_IGZIP
  EndCompressIGZIP(settings->isal_strm);
#endif
  settings->isal_strm = nullptr;
}

static void ReleaseInflateIgzipState(
    const std::shared_ptr<InflateSettings>& settings) {
  if (settings == nullptr || settings->isal_strm == nullptr) {
    return;
  }
#ifdef USE_IGZIP
  EndUncompressIGZIP(settings->isal_strm);
#endif
  settings->isal_strm = nullptr;
}

class DeflateStreamSettings {
 public:
  void Set(z_streamp strm, int level, int method, int window_bits,
           int mem_level, int strategy) {
    auto previous = map.Get(strm);
    auto settings = std::make_shared<DeflateSettings>(
        level, method, window_bits, mem_level, strategy);
    map.Set(strm, std::move(settings));
    // A second deflateInit*() on a stream that was never ended replaces an
    // entry that may still own an ISA-L stream, which nothing can reach once
    // the entry is gone. See SetFromCopy() for the ordering.
    ReleaseDeflateIgzipState(previous);
  }

  // Registers dest as a copy of an already-tracked stream, for deflateCopy().
  // Deliberately not a copy of the DeflateSettings object: that would carry
  // isal_strm over and leave the two streams sharing one ISA-L state.
  //
  // Reports failure instead of throwing: the caller is an exported zlib symbol,
  // so an exception escaping here would cross into a C caller that cannot catch
  // it. The catch is deliberately unqualified -- besides bad_alloc from the
  // allocations here, ShardedMap::Set() locks a std::shared_mutex on the
  // non-TBB build and so can throw std::system_error. Returning false lets the
  // caller undo the copy and report Z_MEM_ERROR, which alongside
  // Z_STREAM_ERROR is the only failure zlib documents for deflateCopy().
  bool SetFromCopy(z_streamp dest, const DeflateSettings& source) {
    // dest may already be an initialized, used stream whose entry owns an ISA-L
    // stream. Read that entry before replacing it, and release it only after
    // the replacement has landed: freeing first would leave a dangling
    // isal_strm in the map if the work below throws. The shared_ptr keeps the
    // old settings alive past map.Set().
    auto previous = map.Get(dest);
    try {
      auto settings = std::make_shared<DeflateSettings>(
          source.level, source.method, source.window_bits, source.mem_level,
          source.strategy);
      settings->path = source.path;
      settings->stream_end_reached = source.stream_end_reached;
      map.Set(dest, std::move(settings));
    } catch (...) {
      Log(LogLevel::LOG_ERROR,
          "SetFromCopy() failed to register deflate stream ",
          static_cast<void*>(dest), "\n");
      return false;
    }
    ReleaseDeflateIgzipState(previous);
    return true;
  }

  void Unset(z_streamp strm) { map.Unset(strm); }

  std::shared_ptr<DeflateSettings> Get(z_streamp strm) { return map.Get(strm); }

  void Init() { map.Init(); }

 private:
  ShardedMap<z_streamp, std::shared_ptr<DeflateSettings>> map;
};
DeflateStreamSettings deflate_stream_settings;

class InflateStreamSettings {
 public:
  void Set(z_streamp strm, int window_bits) {
    auto previous = map.Get(strm);
    auto settings = std::make_shared<InflateSettings>(window_bits);
    map.Set(strm, std::move(settings));
    // See the deflate-side Set().
    ReleaseInflateIgzipState(previous);
  }

  // Registers dest as a copy of an already-tracked stream, for inflateCopy().
  // isal_clone is passed in rather than copied from source so that ownership of
  // the cloned ISA-L state is explicit: the caller allocates it, this entry
  // point hands it to the new settings, and inflateEnd() frees it. Ownership
  // therefore transfers only when this returns true; on false the clone is
  // still the caller's to free, along with zlib's half of the copy. See the
  // deflate-side comment for why failure is reported, not thrown, and for the
  // ordering of the release below.
  bool SetFromCopy(z_streamp dest, const InflateSettings& source,
                   struct inflate_state* isal_clone) {
    auto previous = map.Get(dest);
    try {
      auto settings = std::make_shared<InflateSettings>(source.window_bits);
      settings->path = source.path;
      settings->isal_strm = isal_clone;
      settings->stream_end_reached = source.stream_end_reached;
      map.Set(dest, std::move(settings));
    } catch (...) {
      Log(LogLevel::LOG_ERROR,
          "SetFromCopy() failed to register inflate stream ",
          static_cast<void*>(dest), "\n");
      return false;
    }
    ReleaseInflateIgzipState(previous);
    return true;
  }

  void Unset(z_streamp strm) { map.Unset(strm); }

  std::shared_ptr<InflateSettings> Get(z_streamp strm) { return map.Get(strm); }

  void Init() { map.Init(); }

 private:
  ShardedMap<z_streamp, std::shared_ptr<InflateSettings>> map;
};
InflateStreamSettings inflate_stream_settings;

static void SetDeflatePath(const std::shared_ptr<DeflateSettings>& settings,
                           ExecutionPath new_path) {
  if (settings == nullptr || settings->path == new_path) {
    return;
  }
  settings->path = new_path;
}

static void SetInflatePath(const std::shared_ptr<InflateSettings>& settings,
                           ExecutionPath new_path) {
  if (settings == nullptr || settings->path == new_path) {
    return;
  }
  settings->path = new_path;
}

// Shim-side state work every deflate reset entry point performs. Only the path
// is cleared. zlib's deflateReset keeps the compression level and strategy,
// including any set later by deflateParams(), so the recorded level must
// survive a reset too or path selection would disagree with the level zlib is
// actually using. The terminal-state flag has to go, though: a reset stream is
// ready to compress again, and leaving it set would wedge every later deflate()
// at Z_STREAM_END.
static void ResetDeflateStreamState(
    const std::shared_ptr<DeflateSettings>& settings) {
  if (settings == nullptr) {
    return;
  }
  SetDeflatePath(settings, UNDEFINED);
  settings->stream_end_reached = false;

#ifdef USE_IGZIP
  if (settings->isal_strm != nullptr) {
    // Keeping the recorded level is not sufficient for the ISA-L stream:
    // isal_deflate_reset() deliberately preserves level and level_buf, and
    // deflate() only calls InitCompressIGZIP() when isal_strm is null, so a
    // level that deflateParams() changed since this stream was built would
    // leave the next stream running at the old ISA-L level. Discard the stream
    // in that case and let deflate() rebuild it from the current setting; the
    // common reset, where the level did not change, keeps the stream and its
    // level_buf allocation. The reverse ordering -- reset first, then
    // deflateParams() -- is handled in deflateParams().
    if (CompressLevelChangedIGZIP(settings->isal_strm, settings->level)) {
      EndCompressIGZIP(settings->isal_strm);
      settings->isal_strm = nullptr;
    } else {
      ResetCompressIGZIP(settings->isal_strm);
    }
  }
#endif
}

// Same for the inflate side. A reset stream is ready to decode again; leaving
// the terminal state set would wedge every later inflate() at Z_STREAM_END.
static void ResetInflateStreamState(
    const std::shared_ptr<InflateSettings>& settings) {
  if (settings == nullptr) {
    return;
  }
  SetInflatePath(settings, UNDEFINED);
  settings->stream_end_reached = false;
  if (settings->isal_strm != nullptr) {
#ifdef USE_IGZIP
    ResetUncompressIGZIP(settings->isal_strm);
#endif
  }
}

// zlib's Z_NO_COMPRESSION (0) asks for stored, uncompressed deflate blocks. No
// backend can produce those: ISA-L's level 0 is still LZ77+Huffman ("fastest"),
// and QAT and IAA take no level argument at all -- all three would silently
// compress data the caller asked to be stored. Levels outside zlib's -1..9
// range are rejected by zlib itself, so leave those for zlib to report too.
// Everything else maps onto an ISA-L level in InitCompressIGZIP().
static bool IsOffloadableCompressionLevel(int level) {
  return level == Z_DEFAULT_COMPRESSION || (level >= 1 && level <= 9);
}

// True when ISA-L holds live state for this stream: it has emitted a header and
// may still hold unflushed data, so the stream can neither be handed to zlib
// nor rebuilt at a different compression level without corrupting the output.
// Both deflate()'s level-0 pin and deflateParams()' discard of a stream built
// for a superseded level exempt such a stream, and deflate() uses it to keep an
// already-started IGZIP stream on IGZIP.
static bool IgzipOwnsDeflateStream(
    const std::shared_ptr<DeflateSettings>& settings) {
  return settings != nullptr && settings->path == IGZIP &&
         settings->isal_strm != nullptr;
}

// Z_BLOCK and Z_TREES ask inflate() to stop early -- at the next deflate block
// boundary, and additionally at the end of each block header -- and to report
// the bit position reached in z_stream.data_type. No backend can do either.
// QAT and IAA decompress whole streams in one submission with no notion of a
// block boundary, and ISA-L transits ISAL_BLOCK_NEW_HDR/ISAL_BLOCK_HDR inside
// a single isal_inflate() call with no way to stop there. Left offloaded, such
// a call over-delivers -- it returns the whole stream instead of one block and
// leaves data_type untouched -- so the output is a correct prefix but the bit
// accounting the caller asked for is silently missing. Applications pass these
// values to append to, splice, or randomly access deflate streams, i.e. the
// accounting *is* the request. zlib is the only implementation here that can
// honor it, so route the stream there.
static bool IsOffloadableInflateFlush(int flush) {
  return flush != Z_BLOCK && flush != Z_TREES;
}

int ZEXPORT deflateInit_(z_streamp strm, int level, const char* version,
                         int stream_size) {
  Log(LogLevel::LOG_INFO, "deflateInit_ Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", level ", level, "\n");

  // The shim has no deflate implementation of its own, so a missing symbol is
  // unrecoverable for this entry point. Report it the way zlib reports an
  // unusable library and register nothing, which keeps deflate() off this
  // stream.
  if (orig_deflateInit_ == nullptr) {
    return Z_VERSION_ERROR;
  }

  // Register only once zlib has accepted the stream. On failure the app never
  // calls deflateEnd, so an entry made here would outlive the z_streamp; and
  // zlib leaves an already-initialized stream untouched when it rejects new
  // parameters, so the previous settings must stay in place.
  int ret = orig_deflateInit_(strm, level, version, stream_size);
  if (ret == Z_OK) {
    deflate_stream_settings.Set(strm, level, Z_DEFLATED, kWindowBitsZlib, 8,
                                Z_DEFAULT_STRATEGY);
  }
  return ret;
}

int ZEXPORT deflateInit2_(z_streamp strm, int level, int method,
                          int window_bits, int mem_level, int strategy,
                          const char* version, int stream_size) {
  Log(LogLevel::LOG_INFO, "deflateInit2_ Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", level ", level, ", window_bits ",
      window_bits, " \n");

  if (orig_deflateInit2_ == nullptr) {
    return Z_VERSION_ERROR;
  }

  int ret = orig_deflateInit2_(strm, level, method, window_bits, mem_level,
                               strategy, version, stream_size);
  if (ret == Z_OK) {
    deflate_stream_settings.Set(strm, level, method, window_bits, mem_level,
                                strategy);
  }
  return ret;
}

int ZEXPORT deflateSetDictionary(z_streamp strm, const Bytef* dictionary,
                                 uInt dictLength) {
  if (!configs[IGNORE_ZLIB_DICTIONARY]) {
    Log(LogLevel::LOG_INFO, "deflateSetDictionary Line ", __LINE__, ", strm ",
        static_cast<void*>(strm), ", dictLength ", dictLength, "\n");
    auto deflate_settings = deflate_stream_settings.Get(strm);
    // Reject mid-stream: if an accelerator is active, the underlying zlib
    // stream has not been advanced, so orig_deflateSetDictionary would
    // incorrectly accept the call. Per zlib spec, dictionary must be set before
    // compression begins.
    if (deflate_settings != nullptr && deflate_settings->path != UNDEFINED &&
        deflate_settings->path != ZLIB) {
      return Z_STREAM_ERROR;
    }
    if (orig_deflateSetDictionary == nullptr) {
      return Z_VERSION_ERROR;
    }
    const int ret = orig_deflateSetDictionary(strm, dictionary, dictLength);
    if (ret == Z_OK) {
      SetDeflatePath(deflate_settings, ZLIB);
    }
    return ret;
  }
  Log(LogLevel::LOG_INFO, "deflateSetDictionary Line ", __LINE__,
      " ignored because ignore_zlib_dictionary is set to ",
      configs[IGNORE_ZLIB_DICTIONARY], "\n");
  return Z_OK;
}

int ZEXPORT deflateParams(z_streamp strm, int level, int strategy) {
  Log(LogLevel::LOG_INFO, "deflateParams Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", level ", level, ", strategy ", strategy,
      "\n");
  if (orig_deflateParams == nullptr) {
    return Z_VERSION_ERROR;
  }
  const int ret = orig_deflateParams(strm, level, strategy);
  // On Z_BUF_ERROR zlib documents the parameters as unchanged, so only record
  // them when zlib actually accepted the change -- the same ret == Z_OK gating
  // that deflateInit*() and both *SetDictionary() functions use.
  if (ret == Z_OK) {
    auto deflate_settings = deflate_stream_settings.Get(strm);
    if (deflate_settings != nullptr) {
      deflate_settings->level = level;
      deflate_settings->strategy = strategy;

#ifdef USE_IGZIP
      // deflateReset() gives up an ISA-L stream built for a level that has
      // since changed, but the two orderings need separate handling: reset
      // first and the level still matches at that point, so the stream is kept,
      // and then this call changes the level under a stream deflate() will
      // reuse as-is (it only builds one when isal_strm is null). Discard it
      // here so the next deflate() rebuilds it at the level just requested.
      //
      // A stream ISA-L already owns is the one case to leave alone: it holds a
      // header plus unflushed data, so it cannot be rebuilt mid-stream. That
      // leaves the new level unhonored until the next reset, the same
      // deliberate mid-stream residual as the level-0 pin's exemption in
      // deflate().
      if (!IgzipOwnsDeflateStream(deflate_settings) &&
          deflate_settings->isal_strm != nullptr &&
          CompressLevelChangedIGZIP(deflate_settings->isal_strm, level)) {
        EndCompressIGZIP(deflate_settings->isal_strm);
        deflate_settings->isal_strm = nullptr;
      }
#endif
    }
  }
  return ret;
}

int ZEXPORT deflate(z_streamp strm, int flush) {
  auto deflate_settings = deflate_stream_settings.Get(strm);
  INCREMENT_STAT(DEFLATE_COUNT);
  PrintStats();

  // Without per-stream settings there is nothing to base a path decision on, so
  // hand the call straight to zlib.
  if (deflate_settings == nullptr) {
    return orig_deflate != nullptr ? orig_deflate(strm, flush)
                                   : Z_VERSION_ERROR;
  }

  Log(LogLevel::LOG_INFO, "deflate Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", avail_in ", strm->avail_in, ", avail_out ",
      strm->avail_out, ", flush ", flush, ", in_call ", in_call, ", path ",
      static_cast<int>(deflate_settings->path), ", path_name ",
      ExecutionPathName(deflate_settings->path), ", window_bits ",
      deflate_settings->window_bits, ", total_in ", strm->total_in,
      ", total_out ", strm->total_out, ", adler ", strm->adler, "\n");

  // A stream an accelerator already finished has to be refused here, above both
  // path selection and the zlib fall-through: the offload never fed zlib's own
  // deflate state, so orig_deflate() would see a stream still at INIT_STATE and
  // emit a second header (or a whole empty stream) after a finished one. Reply
  // with what zlib replies once its own state is at FINISH_STATE.
  if (deflate_settings->stream_end_reached) {
    // Same tests in the same order as zlib's deflate(), which validates its
    // parameters before reporting the terminal state. strm->msg follows zlib
    // too: it is only written where zlib rejects through ERR_RETURN, and the
    // strings are the ones that macro would pick (z_errmsg[] in zutil.c).
    int ret = Z_STREAM_END;
    if (flush > Z_BLOCK || flush < 0) {
      // The flush range is zlib's first check and a plain return, not an
      // ERR_RETURN, so an out-of-range value leaves msg as the caller left it.
      ret = Z_STREAM_ERROR;
    } else if (strm->next_out == nullptr ||
               (strm->avail_in != 0 && strm->next_in == nullptr) ||
               flush != Z_FINISH) {
      // Once the stream is finished no flush but Z_FINISH is accepted. zlib
      // rejects all three of these in one ERR_RETURN.
      strm->msg = const_cast<char*>("stream error");
      ret = Z_STREAM_ERROR;
    } else if (strm->avail_out == 0) {
      strm->msg = const_cast<char*>("buffer error");
      ret = Z_BUF_ERROR;
    } else if (strm->avail_in != 0) {
      // "user must not provide more input after the first FINISH".
      strm->msg = const_cast<char*>("buffer error");
      ret = Z_BUF_ERROR;
    }
    Log(LogLevel::LOG_INFO, "deflate Line ", __LINE__, ", strm ",
        static_cast<void*>(strm), ", stream already ended, return code ", ret,
        "\n");
    INCREMENT_STAT(DEFLATE_STREAM_END_COUNT);
    INCREMENT_STAT_COND(ret < 0, DEFLATE_ERROR_COUNT);
    return ret;
  }

  // Everything below reads next_in and next_out -- the offload hands both to a
  // vendor library -- while zlib rejects a null pointer with data behind it
  // before it looks at anything else. Delegate such a call so zlib produces
  // that rejection instead of the shim dereferencing what zlib is about to
  // refuse. zlib's parameter checks touch no stream state, so a delegated call
  // is indistinguishable from an unshimmed one.
  // Counted like the fall-through below rather than like an early exit: the
  // call did reach zlib, and its rejection is an error the statistics should
  // show.
  if (strm->next_out == nullptr ||
      (strm->avail_in != 0 && strm->next_in == nullptr)) {
    if (orig_deflate == nullptr) {
      return Z_VERSION_ERROR;
    }
    const int ret = orig_deflate(strm, flush);
    INCREMENT_STAT(DEFLATE_ZLIB_COUNT);
    INCREMENT_STAT_COND(ret < 0, DEFLATE_ERROR_COUNT);
    return ret;
  }

  // The compression level is a property of the whole stream, not of one call,
  // so decide it here rather than discovering it when InitCompressIGZIP()
  // rejects the level. Pinning the path (rather than only clearing
  // igzip_available) is what lets the stream reach orig_deflate even when
  // use_zlib_compress=0: the request was never an offload candidate, so this is
  // not a fallback -- the same reasoning that pins a dictionary stream to ZLIB.
  // deflateReset() clears the path, so this has to run per call, not at init.
  //
  // A stream ISA-L has already started is the one case that must not be pinned.
  // deflateParams() can lower the level to 0 mid-stream, but by then ISA-L has
  // emitted a header plus compressed data and still holds unflushed state, so
  // handing the stream to a zlib deflate state that was never fed emits a
  // second header and produces output that does not inflate (Z_DATA_ERROR, only
  // the pre-switch bytes recoverable). Staying on IGZIP leaves the new level
  // unhonored -- output is still valid, round-trippable deflate -- which is the
  // same deliberate mid-stream residual as deflate()'s Z_BLOCK -> Z_SYNC_FLUSH
  // aliasing. Documented in the README.
  if (!IsOffloadableCompressionLevel(deflate_settings->level) &&
      !IgzipOwnsDeflateStream(deflate_settings)) {
    SetDeflatePath(deflate_settings, ZLIB);
  }

  int ret = 1;
  bool iaa_available = false;
  bool qat_available = false;
  bool igzip_available = false;
  if (!in_call && deflate_settings->path != ZLIB) {
    uint32_t input_len = strm->avail_in;
    uint32_t output_len = strm->avail_out;
    bool igzip_stream_active = false;

#ifdef USE_IAA
    iaa_available = (flush == Z_FINISH) && configs[USE_IAA_COMPRESS] &&
                    SupportedOptionsIAA(deflate_settings->window_bits,
                                        input_len, output_len);
#endif
#ifdef USE_QAT
    qat_available =
        (flush == Z_FINISH) && configs[USE_QAT_COMPRESS] &&
        output_len >= QAT_DEST_BUFFER_MIN_SIZE &&
        SupportedOptionsQAT(deflate_settings->window_bits, input_len);
#endif
#ifdef USE_IGZIP
    igzip_stream_active = IgzipOwnsDeflateStream(deflate_settings);
    igzip_available =
        configs[USE_IGZIP_COMPRESS] && SupportedOptionsIGZIPDeflate(flush);
#endif

    // If both accelerators are enabled, send configured ratio of requests to
    // one or the other
    ExecutionPath path_selected = ZLIB;
    if (igzip_stream_active) {
      path_selected = IGZIP;
    } else if (iaa_available && qat_available) {
      if (static_cast<uint32_t>(std::rand() % 100) <
          configs[IAA_COMPRESS_PERCENTAGE]) {
        path_selected = IAA;
      } else {
        path_selected = QAT;
      }
    } else if (iaa_available) {
      path_selected = IAA;
    } else if (qat_available) {
      path_selected = QAT;
    } else if (igzip_available) {
      path_selected = IGZIP;
    }

    if (path_selected == IAA) {
#ifdef USE_IAA
      in_call = true;
      // Casting to uint32_t is safe, as IAA is not used for any blocks larger
      // than 2MB
      uint32_t max_compressed_size = (uint32_t)deflateBound(strm, input_len);
      ret = CompressIAA(strm->next_in, &input_len, strm->next_out, &output_len,
                        qpl_path_hardware, deflate_settings->window_bits,
                        max_compressed_size);
      SetDeflatePath(deflate_settings, IAA);
      in_call = false;
      INCREMENT_STAT(DEFLATE_IAA_COUNT);
      INCREMENT_STAT_COND(ret != 0, DEFLATE_IAA_ERROR_COUNT);
#endif  // USE_IAA
    } else if (path_selected == QAT) {
#ifdef USE_QAT
      in_call = true;
      ret = CompressQAT(strm->next_in, &input_len, strm->next_out, &output_len,
                        deflate_settings->window_bits);
      SetDeflatePath(deflate_settings, QAT);
      in_call = false;
      INCREMENT_STAT(DEFLATE_QAT_COUNT);
      INCREMENT_STAT_COND(ret != 0, DEFLATE_QAT_ERROR_COUNT);
#endif  // USE_QAT
    } else if (path_selected == IGZIP) {
#ifdef USE_IGZIP
      if (deflate_settings->isal_strm == nullptr) {
        deflate_settings->isal_strm = InitCompressIGZIP(
            deflate_settings->level, deflate_settings->window_bits);
      }
      if (deflate_settings->isal_strm != nullptr) {
        in_call = true;
        ret = CompressIGZIP(deflate_settings->isal_strm, flush, strm->next_in,
                            &input_len, strm->next_out, &output_len,
                            &strm->total_in, &strm->total_out);
        SetDeflatePath(deflate_settings, IGZIP);
        in_call = false;

        INCREMENT_STAT(DEFLATE_IGZIP_COUNT);
        INCREMENT_STAT_COND(ret != 0, DEFLATE_IGZIP_ERROR_COUNT);
      }
#endif
    }

#ifdef USE_IGZIP
    // Accelerator->IGZIP fallback: if IAA or QAT failed and IGZIP is
    // available, retry with IGZIP before falling through to software zlib.
    if ((path_selected == IAA || path_selected == QAT) && ret != 0 &&
        configs[IGZIP_FALLBACK] && igzip_available) {
      // Accelerator may have modified input_len/output_len on failure.
      // Restore them before retrying with IGZIP.
      input_len = strm->avail_in;
      output_len = strm->avail_out;
      if (deflate_settings->isal_strm == nullptr) {
        deflate_settings->isal_strm = InitCompressIGZIP(
            deflate_settings->level, deflate_settings->window_bits);
      }
      if (deflate_settings->isal_strm != nullptr) {
        in_call = true;
        ret = CompressIGZIP(deflate_settings->isal_strm, flush, strm->next_in,
                            &input_len, strm->next_out, &output_len,
                            &strm->total_in, &strm->total_out);
        SetDeflatePath(deflate_settings, IGZIP);
        in_call = false;
        path_selected = IGZIP;  // use IGZIP return-code semantics below
        INCREMENT_STAT(DEFLATE_IGZIP_COUNT);
        INCREMENT_STAT_COND(ret != 0, DEFLATE_IGZIP_ERROR_COUNT);
      }
    }
#endif  // USE_IGZIP accelerator fallback

    if (ret == 0) {
      strm->next_in += input_len;
      strm->avail_in -= input_len;
      strm->total_in += input_len;
      strm->next_out += output_len;
      strm->avail_out -= output_len;
      strm->total_out += output_len;
      if (path_selected == IGZIP) {
        const bool no_progress = (input_len == 0 && output_len == 0);
        bool finish_done = false;
#ifdef USE_IGZIP
        finish_done = (flush == Z_FINISH) &&
                      IsIGZIPDeflateFinished(deflate_settings->isal_strm);
#endif

        if (finish_done) {
          ret = Z_STREAM_END;
        } else if (!no_progress) {
          ret = Z_OK;
        } else {
          ret = Z_BUF_ERROR;
        }
      } else {
        if (strm->avail_in == 0) {
          ret = Z_STREAM_END;
        } else {
          ret = Z_BUF_ERROR;
        }
      }

      // Remember the completion the accelerator just reported: zlib's own
      // deflate state was never fed, so nothing else records that this stream
      // is finished.
      if (ret == Z_STREAM_END) {
        deflate_settings->stream_end_reached = true;
      }

      Log(LogLevel::LOG_INFO, "deflate Line ", __LINE__, ", strm ",
          static_cast<void*>(strm), ", accelerator return code ", ret,
          ", bytes_in ", input_len, ", bytes_out ", output_len, ", avail_in ",
          strm->avail_in, ", avail_out ", strm->avail_out, ", path ",
          static_cast<int>(deflate_settings->path), ", path_name ",
          ExecutionPathName(deflate_settings->path), "\n");
      return ret;
    }
  }

  if (in_call || configs[USE_ZLIB_COMPRESS] || deflate_settings->path == ZLIB) {
    // Distinguish "no zlib to delegate to" from "zlib rejected the data": the
    // former is an unusable library, not a data problem.
    if (orig_deflate == nullptr) {
      ret = Z_VERSION_ERROR;
    } else {
      ret = orig_deflate(strm, flush);
      INCREMENT_STAT(DEFLATE_ZLIB_COUNT);
      if (!in_call) {
        SetDeflatePath(deflate_settings, ZLIB);
      }
    }
  } else {
    ret = Z_DATA_ERROR;
  }

  Log(LogLevel::LOG_INFO, "deflate Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", zlib return code ", ret, ", avail_in ",
      strm->avail_in, ", avail_out ", strm->avail_out, ", path ",
      static_cast<int>(deflate_settings->path), ", path_name ",
      ExecutionPathName(deflate_settings->path), "\n");

  INCREMENT_STAT_COND(ret < 0, DEFLATE_ERROR_COUNT);
  return ret;
}

int ZEXPORT deflateEnd(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "deflateEnd Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), "\n");
  auto deflate_settings = deflate_stream_settings.Get(strm);
  ReleaseDeflateIgzipState(deflate_settings);
  deflate_stream_settings.Unset(strm);
  return orig_deflateEnd != nullptr ? orig_deflateEnd(strm) : Z_VERSION_ERROR;
}

// zlib builds deflateReset() on top of deflateResetKeep(), so on a libz whose
// internal calls are interposable this wrapper runs nested inside
// orig_deflateReset(). Acting only after the original returns keeps the outer
// wrapper's state work last, so the two orderings agree. Same shape as
// deflateParams() and inflateReset2().
int ZEXPORT deflateReset(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "deflateReset Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), "\n");

  if (orig_deflateReset == nullptr) {
    return Z_VERSION_ERROR;
  }

  const int ret = orig_deflateReset(strm);
  if (ret == Z_OK) {
    ResetDeflateStreamState(deflate_stream_settings.Get(strm));
  }
  return ret;
}

// The other entry point that restarts a finished stream: deflateReset() is
// deflateResetKeep() plus lm_init(), so an application can reach it directly
// and a terminal state left set here would wedge the stream at Z_STREAM_END.
// What it keeps -- the LZ77 window and hash -- only affects how zlib would
// encode the next stream, not whether the shim may offload it: an offloaded
// stream emits no back-references into the previous one, which is a
// self-contained stream any decoder accepts. So no path pin here, unlike
// inflateResetKeep().
int ZEXPORT deflateResetKeep(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "deflateResetKeep Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), "\n");

  if (orig_deflateResetKeep == nullptr) {
    return Z_VERSION_ERROR;
  }

  const int ret = orig_deflateResetKeep(strm);
  if (ret == Z_OK) {
    ResetDeflateStreamState(deflate_stream_settings.Get(strm));
  }
  return ret;
}

int ZEXPORT deflateCopy(z_streamp dest, z_streamp source) {
  Log(LogLevel::LOG_INFO, "deflateCopy Line ", __LINE__, ", dest ",
      static_cast<void*>(dest), ", source ", static_cast<void*>(source), "\n");

  auto deflate_settings = deflate_stream_settings.Get(source);

  // Refuse while ISA-L owns the source stream and has not finished it. A
  // finished stream is copyable: ISA-L is at ZSTATE_END with all output
  // delivered, so there is no state left to duplicate, and the terminal state
  // the copy inherits answers every deflate() on it -- the same handling QAT
  // and IAA already get. zlib's copy duplicates only the zlib deflate state,
  // which on an offloaded stream has never been fed, and ISA-L's state cannot
  // be duplicated alongside it: isal_zstream::level_buf is cast to a private
  // struct holding pointers into its own allocation, so a byte copy would leave
  // both streams writing into one pending block. Draining that block first is
  // no help -- those bytes belong to the prefix the two streams share, and
  // deflateCopy() cannot hand bytes back to the caller. Failing before
  // orig_deflateCopy leaves dest as the caller passed it, the same shape as
  // deflateSetDictionary()'s mid-stream rejection.
  //
  // QAT and IAA need no equivalent check: they offload with Z_FINISH only and
  // commit output only on full consumption, so they never leave a stream
  // mid-stream. That is how the libraries behave rather than what they promise
  // -- qatzip.h permits a partial result, and CompressIAA does not compare
  // job->total_in to available_in -- so it is worth re-checking after a QATzip
  // or QPL upgrade. A plain follow-up deflate() mishandles such a state
  // identically, so the gap would not be specific to copying.
  if (IgzipOwnsDeflateStream(deflate_settings) &&
      !deflate_settings->stream_end_reached) {
    Log(LogLevel::LOG_INFO, "deflateCopy Line ", __LINE__,
        " rejected, ISA-L holds live state for source stream\n");
    return Z_STREAM_ERROR;
  }

  // deflateEnd is required as well as deflateCopy: registering dest below can
  // fail, and undoing zlib's half of the copy is the only way to avoid handing
  // back a destination the shim does not know about. Refusing here, before
  // anything is allocated, keeps that rollback unconditional.
  if (orig_deflateCopy == nullptr || orig_deflateEnd == nullptr) {
    return Z_VERSION_ERROR;
  }

  int ret = orig_deflateCopy(dest, source);
  if (ret == Z_OK && deflate_settings != nullptr) {
    // The copy inherits the source's path along with its init parameters: a
    // source pinned to ZLIB -- by Z_NO_COMPRESSION or a preset dictionary --
    // was pinned because the request was never offloadable, which is just as
    // true of the copy, and leaving the copy UNDEFINED would re-run path
    // selection on a stream that is already under way. No ISA-L stream is
    // carried over: a live one was refused above, one merely kept across
    // deflateReset() is freshly reset, and one belonging to a finished stream
    // has nothing left to give, so deflate() rebuilds it lazily at the recorded
    // level after a reset clears the terminal state.
    if (!deflate_stream_settings.SetFromCopy(dest, *deflate_settings)) {
      // Undo zlib's half of the copy rather than hand back a destination the
      // shim does not know about: an untracked copy degrades to orig_deflate on
      // a state the accelerator never fed, and a caller told the copy failed
      // will not call deflateEnd() to release it.
      orig_deflateEnd(dest);
      return Z_MEM_ERROR;
    }
  }
  return ret;
}

int ZEXPORT inflateInit_(z_streamp strm, const char* version, int stream_size) {
  Log(LogLevel::LOG_INFO, "inflateInit_ Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), "\n");

  if (orig_inflateInit_ == nullptr) {
    return Z_VERSION_ERROR;
  }

  int ret = orig_inflateInit_(strm, version, stream_size);
  if (ret == Z_OK) {
    inflate_stream_settings.Set(strm, kWindowBitsZlib);
  }
  return ret;
}

int ZEXPORT inflateInit2_(z_streamp strm, int window_bits, const char* version,
                          int stream_size) {
  Log(LogLevel::LOG_INFO, "inflateInit2_ Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", window_bits ", window_bits, "\n");

  if (orig_inflateInit2_ == nullptr) {
    return Z_VERSION_ERROR;
  }

  int ret = orig_inflateInit2_(strm, window_bits, version, stream_size);
  if (ret == Z_OK) {
    inflate_stream_settings.Set(strm, window_bits);
  }
  return ret;
}

int ZEXPORT inflateSetDictionary(z_streamp strm, const Bytef* dictionary,
                                 uInt dictLength) {
  if (!configs[IGNORE_ZLIB_DICTIONARY]) {
    Log(LogLevel::LOG_INFO, "inflateSetDictionary Line ", __LINE__, ", strm ",
        static_cast<void*>(strm), ", dictLength ", dictLength, "\n");
    auto inflate_settings = inflate_stream_settings.Get(strm);
    if (orig_inflateSetDictionary == nullptr) {
      return Z_VERSION_ERROR;
    }
    const int ret = orig_inflateSetDictionary(strm, dictionary, dictLength);
    if (ret == Z_OK) {
      SetInflatePath(inflate_settings, ZLIB);
    }
    return ret;
  }
  Log(LogLevel::LOG_INFO, "inflateSetDictionary Line ", __LINE__,
      " ignored because ignore_zlib_dictionary is set to ",
      configs[IGNORE_ZLIB_DICTIONARY], "\n");
  return Z_OK;
}

int ZEXPORT inflate(z_streamp strm, int flush) {
  auto inflate_settings = inflate_stream_settings.Get(strm);

  INCREMENT_STAT(INFLATE_COUNT);
  PrintStats();

  // Without per-stream settings there is nothing to base a path decision on, so
  // hand the call straight to zlib.
  if (inflate_settings == nullptr) {
    return orig_inflate != nullptr ? orig_inflate(strm, flush)
                                   : Z_VERSION_ERROR;
  }

  Log(LogLevel::LOG_INFO, "inflate Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", avail_in ", strm->avail_in, ", avail_out ",
      strm->avail_out, ", flush ", flush, ", in_call ", in_call, ", path ",
      static_cast<int>(inflate_settings->path), ", path_name ",
      ExecutionPathName(inflate_settings->path), ", window_bits ",
      inflate_settings->window_bits, ", total_in ", strm->total_in,
      ", total_out ", strm->total_out, ", adler ", strm->adler, "\n");

  PrintDeflateBlockHeader(LogLevel::LOG_INFO, strm->next_in, strm->avail_in,
                          inflate_settings->window_bits);

  // A stream an accelerator already carried to its end has to be refused here,
  // above path selection and the zlib fall-through both: zlib's own inflate
  // state never saw the compressed data, so orig_inflate() would decode the
  // whole stream a second time. zlib reports Z_STREAM_END for every flush once
  // it is done, consuming no input and writing no output.
  if (inflate_settings->stream_end_reached) {
    // zlib's inflate() validates these two before looking at its state, and
    // unlike deflate() it does not mind avail_out == 0 once it is done. Both
    // are plain returns there rather than ERR_RETURNs -- inflate() never sets
    // strm->msg on a parameter rejection -- so msg is left as the caller left
    // it. Nor is data_type written: no offloaded call can compute it, which the
    // README documents as a limitation of every path rather than of this gate.
    int ret = Z_STREAM_END;
    if (strm->next_out == nullptr ||
        (strm->next_in == nullptr && strm->avail_in != 0)) {
      ret = Z_STREAM_ERROR;
    }
    Log(LogLevel::LOG_INFO, "inflate Line ", __LINE__, ", strm ",
        static_cast<void*>(strm), ", stream already ended, return code ", ret,
        "\n");
    INCREMENT_STAT(INFLATE_STREAM_END_COUNT);
    INCREMENT_STAT_COND(ret < 0, INFLATE_ERROR_COUNT);
    return ret;
  }

  // The zlib-header probe below, the IAA decompressibility probe and the
  // offload itself all read next_in, and zlib rejects a null pointer with data
  // behind it before it looks at anything else. Delegate such a call rather
  // than dereferencing what zlib is about to refuse; its parameter checks touch
  // no stream state, so a delegated call is indistinguishable from an unshimmed
  // one. This also covers a null next_out, which the IGZIP drain below would
  // otherwise hand to ISA-L.
  // Counted like the fall-through below rather than like an early exit, for the
  // same reason as in deflate().
  if (strm->next_out == nullptr ||
      (strm->next_in == nullptr && strm->avail_in != 0)) {
    if (orig_inflate == nullptr) {
      return Z_VERSION_ERROR;
    }
    const int ret = orig_inflate(strm, flush);
    INCREMENT_STAT(INFLATE_ZLIB_COUNT);
    INCREMENT_STAT_COND(ret < 0, INFLATE_ERROR_COUNT);
    return ret;
  }

  int ret = 1;
  bool end_of_stream = true;
  bool iaa_available = false;
  bool qat_available = false;
  bool igzip_available = false;

  const bool igzip_stream_active = (inflate_settings->path == IGZIP &&
                                    inflate_settings->isal_strm != nullptr);

#ifdef USE_IGZIP
  const bool igzip_supported_options =
      !in_call && configs[USE_IGZIP_UNCOMPRESS] &&
      SupportedOptionsIGZIPInflate(inflate_settings->window_bits);

  // Keep stateful IGZIP stream handling on the same engine.
  // For avail_in==0, let IGZIP process any buffered bits in its internal
  // state before reporting Z_BUF_ERROR.
  if (!in_call && igzip_stream_active && strm->avail_in == 0) {
    in_call = true;
    IGZIPHandleActiveStreamNoInput(strm, inflate_settings->isal_strm, &ret);
    in_call = false;
    // Second site where inflate() reports a completion, so it records the
    // terminal state too. Redundant on its own -- ISA-L's inflate_state stays
    // in ISAL_BLOCK_FINISH and answers later calls the way zlib would. It is
    // recorded anyway so the flag means the same thing on every path: once a
    // stream has ended, the gate above answers for it rather than any given
    // backend's state.
    if (ret == Z_STREAM_END) {
      inflate_settings->stream_end_reached = true;
    }
    return ret;
  }
#endif

  // Early detection: if this is a zlib-format stream with the FDICT bit set
  // in the header, pin to ZLIB immediately so dictionary streams never reach
  // any accelerator (QAT/IAA/IGZIP don't support preset dictionaries).
  if (!in_call && inflate_settings->path == UNDEFINED &&
      inflate_settings->window_bits >= 8 &&
      inflate_settings->window_bits <= kWindowBitsZlib && strm->avail_in >= 2 &&
      (strm->next_in[1] & ZLIB_FDICT_MASK)) {
    SetInflatePath(inflate_settings, ZLIB);
  }

  // Z_BLOCK/Z_TREES cannot be honored by any accelerator, so pin the stream to
  // zlib rather than only skipping the offload for this one call. The bit
  // accounting such a caller performs spans the whole stream: letting a later
  // Z_NO_FLUSH call migrate it onto an accelerator would leave data_type unset
  // again mid-stream and break the accounting just as thoroughly. Pinning also
  // reaches orig_inflate when use_zlib_uncompress=0, via the "path == ZLIB"
  // term below -- a request that was never offloadable is not a fallback, the
  // same reasoning that pins a dictionary stream or a level-0 deflate stream.
  //
  // An IGZIP stream already in flight is exempt: ISA-L holds unflushed inflate
  // state that cannot be handed to zlib without corrupting the output, so such
  // a stream keeps behaving as Z_NO_FLUSH. That mirrors the accepted residual
  // on the compress side (see CompressIGZIP's flush mapping); both are
  // documented in the README.
  if (!in_call && !igzip_stream_active && !IsOffloadableInflateFlush(flush)) {
    SetInflatePath(inflate_settings, ZLIB);
  }

  if (!in_call && strm->avail_in > 0 && inflate_settings->path != ZLIB) {
    uint32_t input_len = strm->avail_in;
    uint32_t output_len = strm->avail_out;

#ifdef USE_IAA
    iaa_available = configs[USE_IAA_UNCOMPRESS] &&
                    SupportedOptionsIAA(inflate_settings->window_bits,
                                        input_len, output_len) &&
                    IsIAADecompressible(strm->next_in, input_len,
                                        inflate_settings->window_bits);

#endif
#ifdef USE_QAT
    qat_available =
        configs[USE_QAT_UNCOMPRESS] &&
        SupportedOptionsQAT(inflate_settings->window_bits, input_len);
#endif
#ifdef USE_IGZIP
    igzip_available = igzip_supported_options;
#endif

    // If both accelerators are enabled, send configured ratio of requests to
    // one or the other
    ExecutionPath path_selected = ZLIB;
    if (igzip_stream_active) {
      path_selected = IGZIP;
    } else if (iaa_available && qat_available) {
      if (static_cast<uint32_t>(std::rand()) % 100 <
          configs[IAA_UNCOMPRESS_PERCENTAGE]) {
        path_selected = IAA;
      } else {
        path_selected = QAT;
      }
    } else if (iaa_available) {
      path_selected = IAA;
    } else if (qat_available) {
      path_selected = QAT;
    } else if (igzip_available) {
      path_selected = IGZIP;
    }
    if (path_selected == IAA) {
#ifdef USE_IAA
      in_call = true;
      ret = UncompressIAA(strm->next_in, &input_len, strm->next_out,
                          &output_len, qpl_path_hardware,
                          inflate_settings->window_bits, &end_of_stream);
      SetInflatePath(inflate_settings, IAA);
      // IAA inflate is stateless in this wrapper. If stream end was not
      // reached, use zlib for stateful continuation.
      if (!end_of_stream) {
        ret = 1;
      }
      in_call = false;
      INCREMENT_STAT(INFLATE_IAA_COUNT);
      INCREMENT_STAT_COND(ret != 0, INFLATE_IAA_ERROR_COUNT);
#endif  // USE_IAA
    } else if (path_selected == QAT) {
#ifdef USE_QAT
      in_call = true;
      ret =
          UncompressQAT(strm->next_in, &input_len, strm->next_out, &output_len,
                        inflate_settings->window_bits, &end_of_stream);
      SetInflatePath(inflate_settings, QAT);
      // QATzip does not support stateful decompression
      // Fall back to zlib if end-of-stream not reached in one call
      if (!end_of_stream) {
        ret = 1;
      }
      in_call = false;
      INCREMENT_STAT(INFLATE_QAT_COUNT);
      INCREMENT_STAT_COND(ret != 0, INFLATE_QAT_ERROR_COUNT);
#endif  // USE_QAT
    } else if (path_selected == IGZIP) {
#ifdef USE_IGZIP
      in_call = true;
      const IGZIPInflatePathAction path_action =
          IGZIPRunInflateAndSelectPathAction(
              strm, &inflate_settings->isal_strm, inflate_settings->window_bits,
              &input_len, &output_len, &ret, &end_of_stream);
      in_call = false;

      if (inflate_settings->isal_strm == nullptr) {
        return Z_DATA_ERROR;
      }

      if (path_action == IGZIP_INFLATE_PATH_FALLBACK_NEED_DICT) {
        Log(LogLevel::LOG_ERROR, " strm=", static_cast<void*>(strm),
            " source=igzip", " total_in=", strm->total_in,
            " total_out=", strm->total_out, " adler=", strm->adler, "\n");
        SetInflatePath(inflate_settings, ZLIB);
      } else if (path_action == IGZIP_INFLATE_PATH_FALLBACK_DATA_ERROR) {
        SetInflatePath(inflate_settings, ZLIB);
      } else if (path_action == IGZIP_INFLATE_PATH_SET_IGZIP &&
                 inflate_settings->path != ZLIB) {
        SetInflatePath(inflate_settings, IGZIP);
      }
      INCREMENT_STAT(INFLATE_IGZIP_COUNT);
      INCREMENT_STAT_COND(ret != 0, INFLATE_IGZIP_ERROR_COUNT);
#endif
    }

#ifdef USE_IGZIP
    // Accelerator->IGZIP fallback: if IAA or QAT failed and IGZIP is
    // available, retry with IGZIP before falling through to software zlib.
    if ((path_selected == IAA || path_selected == QAT) && ret != 0 &&
        configs[IGZIP_FALLBACK] && igzip_available) {
      // Accelerator may have modified input_len/output_len on failure.
      // Restore them before retrying with IGZIP.
      input_len = strm->avail_in;
      output_len = strm->avail_out;
      end_of_stream = true;
      in_call = true;
      const IGZIPInflatePathAction path_action =
          IGZIPRunInflateAndSelectPathAction(
              strm, &inflate_settings->isal_strm, inflate_settings->window_bits,
              &input_len, &output_len, &ret, &end_of_stream);
      in_call = false;

      if (inflate_settings->isal_strm == nullptr) {
        return Z_DATA_ERROR;
      }

      if (path_action == IGZIP_INFLATE_PATH_FALLBACK_NEED_DICT) {
        Log(LogLevel::LOG_ERROR, " strm=", static_cast<void*>(strm),
            " source=igzip (", (path_selected == QAT) ? "QAT" : "IAA",
            " fallback)", " total_in=", strm->total_in,
            " total_out=", strm->total_out, " adler=", strm->adler, "\n");
        SetInflatePath(inflate_settings, ZLIB);
      } else if (path_action == IGZIP_INFLATE_PATH_FALLBACK_DATA_ERROR) {
        SetInflatePath(inflate_settings, ZLIB);
      } else if (path_action == IGZIP_INFLATE_PATH_SET_IGZIP &&
                 inflate_settings->path != ZLIB) {
        SetInflatePath(inflate_settings, IGZIP);
      }
      INCREMENT_STAT(INFLATE_IGZIP_COUNT);
      INCREMENT_STAT_COND(ret != 0, INFLATE_IGZIP_ERROR_COUNT);
    }
#endif  // USE_IGZIP accelerator fallback

    if (ret == 0) {
      strm->next_in += input_len;
      strm->avail_in -= input_len;
      strm->total_in += input_len;
      strm->next_out += output_len;
      strm->avail_out -= output_len;
      strm->total_out += output_len;
      if (end_of_stream) {
        ret = Z_STREAM_END;
      } else if (input_len > 0 || output_len > 0) {
        ret = Z_OK;
      } else {
        ret = Z_BUF_ERROR;
      }

      // Remember the completion: zlib's own inflate state never saw this
      // stream, so nothing else records that it ended.
      if (ret == Z_STREAM_END) {
        inflate_settings->stream_end_reached = true;
      }

      Log(LogLevel::LOG_INFO, "inflate Line ", __LINE__, ", strm ",
          static_cast<void*>(strm), ", accelerator return code ", ret,
          ", bytes_in ", input_len, ", bytes_out ", output_len, ", avail_in ",
          strm->avail_in, ", avail_out ", strm->avail_out, ", end_of_stream ",
          end_of_stream, ", path ", static_cast<int>(inflate_settings->path),
          ", path_name ", ExecutionPathName(inflate_settings->path),
          ", window_bits ", inflate_settings->window_bits, "\n");
      return ret;
    }
  }

  // The "path == ZLIB" term is also how an IGZIP stream that hit Z_NEED_DICT or
  // Z_DATA_ERROR reaches zlib: those path actions set the path to ZLIB and
  // leave ret non-zero, so the update block above is skipped and control
  // arrives here with strm->next_in never advanced.  zlib therefore re-reads
  // the original, untouched input.  The fall-through is deliberate, not
  // accidental.
  if (in_call || configs[USE_ZLIB_UNCOMPRESS] ||
      inflate_settings->path == ZLIB) {
    // refer to comment in deflate
    if (orig_inflate == nullptr) {
      ret = Z_VERSION_ERROR;
    } else {
      ret = orig_inflate(strm, flush);
      if (ret == Z_NEED_DICT) {
        Log(LogLevel::LOG_ERROR, " strm=", static_cast<void*>(strm),
            " source=zlib", " total_in=", strm->total_in,
            " total_out=", strm->total_out, " adler=", strm->adler, "\n");
      }
      INCREMENT_STAT(INFLATE_ZLIB_COUNT);
      if (!in_call) {
        SetInflatePath(inflate_settings, ZLIB);
      }
      // A stream pinned to ZLIB cannot return to IGZIP -- igzip_stream_active
      // above is path == IGZIP -- so any ISA-L stream it still owns is
      // unreachable, and goes back here rather than sitting dormant until
      // inflateEnd(). inflateResetKeep() and inflateSetDictionary() both pin
      // streams that may own one; whichever applied the pin, this is where the
      // state is provably out of reach. Gated on the path rather than on
      // !in_call because the path is what makes it unreachable: a reentrant
      // call on some other stream says nothing about this one.
      if (inflate_settings->path == ZLIB) {
        ReleaseInflateIgzipState(inflate_settings);
      }
    }
  } else {
    ret = Z_DATA_ERROR;
  }

  Log(LogLevel::LOG_INFO, "inflate Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", zlib return code ", ret, ", avail_in ",
      strm->avail_in, ", avail_out ", strm->avail_out, ", path ",
      static_cast<int>(inflate_settings->path), ", path_name ",
      ExecutionPathName(inflate_settings->path), ", window_bits ",
      inflate_settings->window_bits, "\n");

  INCREMENT_STAT_COND(ret < 0, INFLATE_ERROR_COUNT);
  return ret;
}

int ZEXPORT inflateEnd(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "inflateEnd Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), "\n");
  auto inflate_settings = inflate_stream_settings.Get(strm);
  ReleaseInflateIgzipState(inflate_settings);
  inflate_stream_settings.Unset(strm);
  return orig_inflateEnd != nullptr ? orig_inflateEnd(strm) : Z_VERSION_ERROR;
}

// inflateReset() is inflateResetKeep() plus a discarded window, so this wrapper
// may run nested inside orig_inflateReset() where libz's internal calls are
// interposable. Acting after the original returns puts this wrapper's state
// work last, which is what makes the path pin inflateResetKeep() applies
// specific to a direct call. Same shape as inflateReset2().
//
// Ordering is enough because the pin is a field this wrapper overwrites. That
// is also why the ISA-L state a pin strands is released in inflate(), where the
// path still says ZLIB, rather than by inflateResetKeep(): a nested free is not
// something this wrapper could undo.
int ZEXPORT inflateReset(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "inflateReset Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), "\n");

  if (orig_inflateReset == nullptr) {
    return Z_VERSION_ERROR;
  }

  const int ret = orig_inflateReset(strm);
  if (ret == Z_OK) {
    ResetInflateStreamState(inflate_stream_settings.Get(strm));
  }
  return ret;
}

// inflateResetKeep() restarts the stream but keeps the window zlib has built,
// so like the other reset entry points it has to clear the terminal state, and
// unlike them it also has to pin the stream to zlib. Retaining the window is
// the only reason to call this rather than inflateReset() -- neither frees it
// -- so a caller that does is saying the next stream may reference the previous
// stream's bytes. That is a preset dictionary in all but name, and no backend
// can see that history: an accelerator would decode the lookback from whatever
// its own window happens to hold. The pin is what inflateSetDictionary() does
// for the same reason, and inflateReset() lifts it, being the reset that
// actually discards the history.
//
// What the pin cannot do is supply the history. If the previous stream was
// offloaded, zlib's own window never received it, so a next stream that really
// does reference those bytes fails in orig_inflate() with Z_DATA_ERROR rather
// than decoding. That is inherent: the bytes exist only in the output the
// accelerator already handed the caller, and whether they will be referenced is
// unknowable while the previous stream is still being decoded. What the pin
// buys is the failure mode -- a zlib data error on the stream that needs the
// history, instead of a backend decoding a lookback against an unrelated window
// and returning success. Rejecting the reset outright would be worse: it fails
// the common history-independent restart, which works, to report the rare case
// earlier. Documented in the README.
//
// Any ISA-L stream the pin puts out of reach is handed back by the next
// inflate(), not here -- see the release at the zlib fall-through.
int ZEXPORT inflateResetKeep(z_streamp strm) {
  Log(LogLevel::LOG_INFO, "inflateResetKeep Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), "\n");

  if (orig_inflateResetKeep == nullptr) {
    return Z_VERSION_ERROR;
  }

  const int ret = orig_inflateResetKeep(strm);
  if (ret == Z_OK) {
    auto inflate_settings = inflate_stream_settings.Get(strm);
    ResetInflateStreamState(inflate_settings);
    SetInflatePath(inflate_settings, ZLIB);
  }
  return ret;
}

// inflateReset2() is the only zlib entry point that changes windowBits on a
// live stream, so it is the only one that can restart a finished stream without
// going through inflateReset(). It has to be intercepted for two reasons: the
// recorded window_bits would otherwise go stale and path selection would keep
// deciding on the format the stream was initialized with, and a stream left
// marked as ended would keep returning Z_STREAM_END forever.
//
// zlib validates windowBits itself and leaves the stream untouched when it is
// invalid, so mirror deflateParams() and only act once the original reports
// success.
int ZEXPORT inflateReset2(z_streamp strm, int windowBits) {
  Log(LogLevel::LOG_INFO, "inflateReset2 Line ", __LINE__, ", strm ",
      static_cast<void*>(strm), ", window_bits ", windowBits, "\n");

  if (orig_inflateReset2 == nullptr) {
    return Z_VERSION_ERROR;
  }

  int ret = orig_inflateReset2(strm, windowBits);
  if (ret != Z_OK) {
    return ret;
  }

  auto inflate_settings = inflate_stream_settings.Get(strm);
  if (inflate_settings != nullptr) {
    ResetInflateStreamState(inflate_settings);
    inflate_settings->window_bits = windowBits;

    if (inflate_settings->isal_strm != nullptr) {
#ifdef USE_IGZIP
      // isal_inflate_reset() deliberately preserves crc_flag and hist_bits, and
      // inflate() only builds a new ISA-L stream when isal_strm is null, so a
      // window or format change has to discard the stream rather than reset it
      // -- otherwise the next call decodes the new format with the old
      // crc_flag. The common case, where only the terminal state and path need
      // clearing, keeps the stream. Same shape as deflateReset()'s handling of
      // a level change.
      if (UncompressWindowChangedIGZIP(inflate_settings->isal_strm,
                                       windowBits)) {
        EndUncompressIGZIP(inflate_settings->isal_strm);
        inflate_settings->isal_strm = nullptr;
      } else {
        ResetUncompressIGZIP(inflate_settings->isal_strm);
      }
#endif
    }
  }

  return ret;
}

int ZEXPORT inflateCopy(z_streamp dest, z_streamp source) {
  Log(LogLevel::LOG_INFO, "inflateCopy Line ", __LINE__, ", dest ",
      static_cast<void*>(dest), ", source ", static_cast<void*>(source), "\n");

  // Both symbols are required, for the reason given in deflateCopy().
  if (orig_inflateCopy == nullptr || orig_inflateEnd == nullptr) {
    return Z_VERSION_ERROR;
  }

  int ret = orig_inflateCopy(dest, source);
  if (ret != Z_OK) {
    return ret;
  }

  auto inflate_settings = inflate_stream_settings.Get(source);
  if (inflate_settings == nullptr) {
    // Untracked source: leave dest untracked too, so it reaches orig_inflate
    // the same way the source does.
    return ret;
  }

  struct inflate_state* isal_clone = nullptr;
#ifdef USE_IGZIP
  // Clone the ISA-L state only when ISA-L is the engine in use. Unlike the
  // deflate side this is exact, mid-stream included: inflate_state is
  // self-contained, and next_in/next_out are re-pointed on every call. A stream
  // on any other path either holds no ISA-L state or will never use it again
  // (ZLIB is sticky), so cloning it would only cost an allocation.
  if (inflate_settings->path == IGZIP &&
      inflate_settings->isal_strm != nullptr) {
    isal_clone = CopyUncompressIGZIP(inflate_settings->isal_strm);
    if (isal_clone == nullptr) {
      // Undo zlib's half of the copy rather than register a stream with no
      // ISA-L state to continue from; otherwise dest leaks zlib's inflate
      // state, since a caller told the copy failed will not call inflateEnd.
      orig_inflateEnd(dest);
      return Z_MEM_ERROR;
    }
  }
#endif

  if (!inflate_stream_settings.SetFromCopy(dest, *inflate_settings,
                                           isal_clone)) {
    // Ownership of the clone never transferred, so free it here, and undo
    // zlib's half of the copy for the same reason as the deflate side.
#ifdef USE_IGZIP
    if (isal_clone != nullptr) {
      EndUncompressIGZIP(isal_clone);
    }
#endif
    orig_inflateEnd(dest);
    return Z_MEM_ERROR;
  }
  return ret;
}

// Note: compress2 / uncompress2 are one-shot stateless paths. They do NOT
// honor IGZIP_FALLBACK: if the preferred accelerator fails, control falls
// directly to software zlib, bypassing IGZIP even when IGZIP_FALLBACK=1.
// This diverges from the streaming deflate()/inflate() paths intentionally —
// stateless paths have no retry loop and the added complexity is not warranted.
int ZEXPORT compress2(Bytef* dest, uLongf* destLen, const Bytef* source,
                      uLong sourceLen, int level) {
  Log(LogLevel::LOG_INFO, "compress2 Line ", __LINE__, ", sourceLen ",
      sourceLen, ", destLen ", *destLen, "\n");

  int ret = 1;
  uint32_t input_len = sourceLen;
  (void)input_len;
  uint32_t output_len = *destLen;

  // One-shot call: there is no per-stream path to pin, so carry the same
  // decision deflate() makes in a local. See IsOffloadableCompressionLevel().
  const bool level_offloadable = IsOffloadableCompressionLevel(level);

  bool iaa_available = false;
  bool qat_available = false;
  bool igzip_available = false;
#ifdef USE_IAA
  iaa_available = level_offloadable && configs[USE_IAA_COMPRESS] &&
                  SupportedOptionsIAA(kWindowBitsZlib, input_len, output_len);
#endif
#ifdef USE_QAT
  qat_available = level_offloadable && configs[USE_QAT_COMPRESS] &&
                  SupportedOptionsQAT(kWindowBitsZlib, input_len);
#endif
#ifdef USE_IGZIP
  // compress2 is one-shot, i.e. equivalent to a single deflate(Z_FINISH).
  igzip_available = level_offloadable && configs[USE_IGZIP_COMPRESS] &&
                    SupportedOptionsIGZIPDeflate(Z_FINISH);
#endif

  ExecutionPath path_selected = ZLIB;
  if (iaa_available) {
    path_selected = IAA;
  } else if (qat_available) {
    path_selected = QAT;
  } else if (igzip_available) {
    path_selected = IGZIP;
  }

  if (path_selected == IAA) {
#ifdef USE_IAA
    in_call = true;
    ret = CompressIAA(const_cast<uint8_t*>(source), &input_len, dest,
                      &output_len, qpl_path_hardware, kWindowBitsZlib);
    in_call = false;
#endif  // USE_IAA
  } else if (path_selected == QAT) {
#ifdef USE_QAT
    in_call = true;
    ret = CompressQAT(const_cast<uint8_t*>(source), &input_len, dest,
                      &output_len, kWindowBitsZlib);
    in_call = false;
#endif  // USE_QAT
  } else if (path_selected == IGZIP) {
#ifdef USE_IGZIP
    in_call = true;
    struct isal_zstream* isal_strm = InitCompressIGZIP(level, kWindowBitsZlib);
    if (isal_strm == nullptr) {
      ret = 1;
    } else {
      unsigned long total_in = 0;
      unsigned long total_out = 0;
      ret = CompressIGZIP(isal_strm, Z_FINISH, const_cast<uint8_t*>(source),
                          &input_len, dest, &output_len, &total_in, &total_out);
      if (ret == 0 && !IsIGZIPDeflateFinished(isal_strm)) {
        // compress2 is one-shot: if the stream did not reach terminal state
        // (ZSTATE_END), the output is incomplete (e.g. trailer truncated by a
        // too-small destLen, or not all input consumed). Mirrors the
        // !end_of_stream check in the uncompress2 IGZIP path.
        ret = 1;
      }
      EndCompressIGZIP(isal_strm);
    }
    in_call = false;
#endif
  }

  if (ret == 0) {
    *destLen = output_len;
    ret = Z_OK;

    Log(LogLevel::LOG_INFO, "compress2 Line ", __LINE__,
        ", accelerator return code ", ret, ", sourceLen ", sourceLen,
        ", destLen ", *destLen, "\n");
  } else if (configs[USE_ZLIB_COMPRESS] || !level_offloadable) {
    // refer to comment in deflate.
    //
    // The !level_offloadable term: a level no backend can honor was never an
    // offload candidate, so reaching zlib here is not a fallback and must not
    // be gated by use_zlib_compress. One-shot analogue of the
    // "deflate_settings->path == ZLIB" term in deflate().
    if (orig_compress2 == nullptr) {
      ret = Z_VERSION_ERROR;
    } else {
      // compress2 in zlib calls deflate. It was observed that deflate is
      // sometimes intercepted by the shim. in_call prevents deflate from using
      // accelerators.
      in_call = true;
      ret = orig_compress2(dest, destLen, source, sourceLen, level);
      in_call = false;
      Log(LogLevel::LOG_INFO, "compress2 Line ", __LINE__,
          ", zlib return code ", ret, ", sourceLen ", sourceLen, ", destLen ",
          *destLen, "\n");
    }
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
  Log(LogLevel::LOG_INFO, "uncompress2 Line ", __LINE__, ", sourceLen ",
      *sourceLen, ", destLen ", *destLen, "\n");

  int ret = 1;
  bool end_of_stream = true;
  (void)end_of_stream;
  uint32_t input_len = *sourceLen;
  uint32_t output_len = *destLen;

  bool iaa_available = false;
  bool qat_available = false;
  bool igzip_available = false;
#ifdef USE_IAA
  iaa_available = configs[USE_IAA_UNCOMPRESS] &&
                  SupportedOptionsIAA(kWindowBitsZlib, input_len, output_len) &&
                  IsIAADecompressible(const_cast<uint8_t*>(source), input_len,
                                      kWindowBitsZlib);
#endif
#ifdef USE_QAT
  qat_available = configs[USE_QAT_UNCOMPRESS] &&
                  SupportedOptionsQAT(kWindowBitsZlib, input_len);
#endif
#ifdef USE_IGZIP
  igzip_available = configs[USE_IGZIP_UNCOMPRESS];
#endif

  ExecutionPath path_selected = ZLIB;
  if (iaa_available) {
    path_selected = IAA;
  } else if (qat_available) {
    path_selected = QAT;
  } else if (igzip_available) {
    path_selected = IGZIP;
  }

  if (path_selected == IAA) {
#ifdef USE_IAA
    in_call = true;
    ret = UncompressIAA(const_cast<uint8_t*>(source), &input_len, dest,
                        &output_len, qpl_path_hardware, kWindowBitsZlib,
                        &end_of_stream);
    if (!end_of_stream) {
      ret = 1;
    }
    in_call = false;
#endif  // USE_IAA
  } else if (path_selected == QAT) {
#ifdef USE_QAT
    in_call = true;
    ret = UncompressQAT(const_cast<uint8_t*>(source), &input_len, dest,
                        &output_len, kWindowBitsZlib, &end_of_stream);
    in_call = false;
#endif  // USE_QAT
  } else if (path_selected == IGZIP) {
#ifdef USE_IGZIP
    in_call = true;
    struct inflate_state* isal_strm = InitUncompressIGZIP(kWindowBitsZlib);
    if (isal_strm == nullptr) {
      ret = 1;
    } else {
      unsigned long total_in = 0;
      unsigned long total_out = 0;
      ret = UncompressIGZIP(isal_strm, const_cast<uint8_t*>(source), &input_len,
                            dest, &output_len, &total_in, &total_out,
                            &end_of_stream);
      EndUncompressIGZIP(isal_strm);
      if (ret == 0 && !end_of_stream) {
        // uncompress2 is one-shot: if ISA-L did not reach end-of-stream,
        // the output is incomplete. end_of_stream is used instead of
        // input_len residual because IGZIP may consume all bytes without
        // seeing the zlib trailer, so checking consumed bytes alone is
        // insufficient. Both checks test the same class of failure (partial
        // decompression) by the most reliable indicator for each path.
        ret = 1;
      }
    }
    in_call = false;
#endif
  }

  if (ret == 0) {
    *sourceLen = input_len;
    *destLen = output_len;
    ret = Z_OK;

    Log(LogLevel::LOG_INFO, "uncompress2 Line ", __LINE__,
        ", accelerator return code ", ret, ", sourceLen ", *sourceLen,
        ", destLen ", *destLen, "\n");
  } else if (configs[USE_ZLIB_UNCOMPRESS]) {
    // refer to comment in deflate
    if (orig_uncompress2 == nullptr) {
      ret = Z_VERSION_ERROR;
    } else {
      // refer to comment in compress2
      in_call = true;
      ret = orig_uncompress2(dest, destLen, source, sourceLen);
      in_call = false;
      Log(LogLevel::LOG_INFO, "uncompress2 Line ", __LINE__,
          ", zlib return code ", ret, ", sourceLen ", *sourceLen, ", destLen ",
          *destLen, "\n");
    }
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
  auto deflate_settings = deflate_stream_settings.Get(strm);
  if (deflate_settings == nullptr) {
    return ZLIB;
  }
  return deflate_settings->path;
}

ExecutionPath GetInflateExecutionPath(z_streamp strm) {
  auto inflate_settings = inflate_stream_settings.Get(strm);
  if (inflate_settings == nullptr) {
    return ZLIB;
  }
  return inflate_settings->path;
}

bool DeflateOwnsIgzipState(z_streamp strm) {
  auto deflate_settings = deflate_stream_settings.Get(strm);
  return deflate_settings != nullptr && deflate_settings->isal_strm != nullptr;
}

bool InflateOwnsIgzipState(z_streamp strm) {
  auto inflate_settings = inflate_stream_settings.Get(strm);
  return inflate_settings != nullptr && inflate_settings->isal_strm != nullptr;
}

enum class FileMode { NONE, READ, WRITE, APPEND };

// What a gzopen/gzdopen mode string asks for beyond the open(2) flags. zlib
// parses all of this out of the same string in gz_open(), so the shim has to as
// well or it acts on settings the application asked for and zlib recorded.
struct GzOpenParams {
  FileMode mode = FileMode::NONE;
  int level = Z_DEFAULT_COMPRESSION;
  // 'T' asks zlib to write the data straight through with no deflate wrapper at
  // all. No backend can do that, and compressing it anyway leaves a gzip file
  // where the caller asked for a copy.
  bool transparent = false;
  // '+' is an error to zlib: it cannot read and write one file at once. It
  // matters here because the shim opens the file itself, before zlib ever sees
  // the mode string, so a mode zlib refuses must not reach open(2) -- O_TRUNC
  // would already have emptied a file that plain zlib leaves untouched.
  bool rejected = false;
};

struct GzipFile {
  GzipFile() { Reset(); }

  GzipFile(int _fd, const GzOpenParams& params)
      : fd(_fd), mode(params.mode), level(params.level) {
    Reset();
    // Two requests no backend can serve, both known at open time. Pin the file
    // to zlib rather than leaving the decision to each write: gzwrite only
    // reaches its zlib branch through path == ZLIB, so anything else would
    // depend on use_zlib_compress being set.
    if (params.transparent || !IsOffloadableCompressionLevel(params.level)) {
      path = ZLIB;
    }
  }

  ~GzipFile() {
    if (data_buf != nullptr) {
      delete[] data_buf;
    }
    if (io_buf != nullptr) {
      delete[] io_buf;
    }
    if (orig_deflateEnd != nullptr) {
      orig_deflateEnd(&deflate_stream);
    }
    if (orig_inflateEnd != nullptr) {
      orig_inflateEnd(&inflate_stream);
    }
  }

  void Reset() {
    path = UNDEFINED;
    use_zlib_for_decompression = false;
    reached_eof = false;
    pushback = -1;

    data_buf_pos = 0;
    data_buf_content = 0;
    io_buf_pos = 0;
    io_buf_content = 0;

    memset(&deflate_stream, 0, sizeof(z_stream));
    if (orig_deflateInit2_ != nullptr) {
      orig_deflateInit2_(&deflate_stream, level, Z_DEFLATED, kWindowBitsGzip, 8,
                         Z_DEFAULT_STRATEGY, ZLIB_VERSION,
                         (int)sizeof(z_stream));
    }
    memset(&inflate_stream, 0, sizeof(z_stream));
    if (orig_inflateInit2_ != nullptr) {
      orig_inflateInit2_(&inflate_stream, kWindowBitsGzip, ZLIB_VERSION,
                         (int)sizeof(z_stream));
    }
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
  // The level the application asked for, through the gzopen mode string or a
  // later gzsetparams. Reset() keeps it, matching zlib, whose reset paths
  // preserve the level too.
  int level = Z_DEFAULT_COMPRESSION;
  // A byte handed back by gzungetc, or -1 for none. It cannot be expressed as a
  // step back in data_buf: the byte pushed need not be the byte read, and there
  // need not have been a read at all. gzread serves it ahead of its own
  // buffers. One byte is what zlib guarantees; a second push before a read may
  // fail, which is what this does.
  int pushback = -1;

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
  void Set(gzFile file, int fd, const GzOpenParams& params) {
    auto f = std::make_shared<GzipFile>(fd, params);
    map.Set(file, std::move(f));
  }

  void Unset(gzFile file) { map.Unset(file); }

  std::shared_ptr<GzipFile> Get(gzFile file) { return map.Get(file); }

  void Init() { map.Init(); }

 private:
  ShardedMap<gzFile, std::shared_ptr<GzipFile>> map;
};
GzipFiles gzip_files;

static void InitStreamRegistries() {
  deflate_stream_settings.Init();
  inflate_stream_settings.Init();
  gzip_files.Init();
}

// Inspired by gz_open in gzlib.c
int GetOpenFlags(const char* mode, GzOpenParams* params) {
  bool cloexec = false;
  bool exclusive = false;

  while (*mode) {
    // The strategy characters ('f', 'h', 'R', 'F') are read and discarded the
    // same way the rest of the shim discards deflateParams' strategy argument.
    if (*mode >= '0' && *mode <= '9') {
      params->level = *mode - '0';
      mode++;
      continue;
    }
    switch (*mode) {
      case 'r':
        params->mode = FileMode::READ;
        break;
      case 'w':
        params->mode = FileMode::WRITE;
        break;
      case 'a':
        params->mode = FileMode::APPEND;
        break;
      case 'b':
        break;
      case 'T':
        params->transparent = true;
        break;
      case '+':
        params->rejected = true;
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
      (params->mode == FileMode::READ
           ? O_RDONLY
           : (O_WRONLY | O_CREAT |
#ifdef O_EXCL
              (exclusive ? O_EXCL : 0) |
#endif
              (params->mode == FileMode::WRITE ? O_TRUNC : O_APPEND)));

  return oflag;
}

gzFile ZEXPORT gzopen(const char* path, const char* mode) {
  // We need to store the file descriptor for use in other functions.
  // Open the file here and then call gzdopen
  if (orig_gzdopen == nullptr) {
    return nullptr;
  }
  GzOpenParams params;
  int oflag = GetOpenFlags(mode, &params);
  // A mode string zlib rejects has to be rejected before open(2), not after:
  // zlib returns NULL without creating or truncating anything, and so must
  // this. FileMode::NONE means the string named no direction, which zlib also
  // refuses.
  if (params.rejected || params.mode == FileMode::NONE) {
    Log(LogLevel::LOG_INFO, "gzopen Line ", __LINE__, ", path ", path,
        ", mode ", mode, " rejected without opening the file\n");
    return nullptr;
  }
  int fd = open((const char*)path, oflag, 0666);
  if (fd < 0) {
    return nullptr;
  }
  gzFile file = orig_gzdopen(fd, mode);
  if (file == nullptr) {
    close(fd);
    return nullptr;
  }

  Log(LogLevel::LOG_INFO, "gzopen Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", path ", path, ", mode ", mode, "\n");

  gzip_files.Set(file, fd, params);
  return file;
}

gzFile ZEXPORT gzdopen(int fd, const char* mode) {
  if (orig_gzdopen == nullptr) {
    return nullptr;
  }
  gzFile file = orig_gzdopen(fd, mode);

  Log(LogLevel::LOG_INFO, "gzdopen Line ", __LINE__, ", file ", fd, ", fd ",
      static_cast<void*>(file), ", mode ", mode, "\n");

  // zlib returns NULL for a mode string it refuses. Registering that would key
  // an entry by NULL, which every gz* entry point then finds when the
  // application passes NULL, in place of the unregistered-file handling.
  if (file == nullptr) {
    return nullptr;
  }

  GzOpenParams params;
  GetOpenFlags(mode, &params);

  gzip_files.Set(file, fd, params);
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
  bool igzip_available = false;

#ifdef USE_IAA
  iaa_available =
      configs[USE_IAA_COMPRESS] &&
      SupportedOptionsIAA(kWindowBitsGzip, *input_length, *output_length);
#endif
#ifdef USE_QAT
  qat_available = configs[USE_QAT_COMPRESS] &&
                  SupportedOptionsQAT(kWindowBitsGzip, *input_length);
#endif
#ifdef USE_IGZIP
  // gzwrite compresses each buffer as a complete stream, i.e. Z_FINISH.
  igzip_available =
      configs[USE_IGZIP_COMPRESS] && SupportedOptionsIGZIPDeflate(Z_FINISH);
#endif

  ExecutionPath path_selected = ZLIB;
  if (qat_available) {
    path_selected = QAT;
  } else if (iaa_available) {
    path_selected = IAA;
  } else if (igzip_available) {
    path_selected = IGZIP;
  }

  if (path_selected == IAA) {
#ifdef USE_IAA
    in_call = true;
    ret = CompressIAA(input, input_length, output, output_length,
                      qpl_path_hardware, kWindowBitsGzip, 0, true);
    gz->path = IAA;
    in_call = false;
#endif  // USE_IAA
  } else if (path_selected == QAT) {
#ifdef USE_QAT
    in_call = true;
    ret = CompressQAT(input, input_length, output, output_length,
                      kWindowBitsGzip, true);
    gz->path = QAT;
    in_call = false;
#endif  // USE_QAT
  } else if (path_selected == IGZIP) {
#ifdef USE_IGZIP
    in_call = true;
    struct isal_zstream* isal_strm =
        InitCompressIGZIP(gz->level, kWindowBitsGzip);
    if (isal_strm == nullptr) {
      ret = 1;
    } else {
      unsigned long total_in = 0;
      unsigned long total_out = 0;
      ret = CompressIGZIP(isal_strm, Z_FINISH, input, input_length, output,
                          output_length, &total_in, &total_out);
      if (ret == 0 && !IsIGZIPDeflateFinished(isal_strm)) {
        // This call site uses IGZIP one-shot: the stream is ended below, so a
        // gzip member left mid-emit can never be completed. isal_deflate
        // reports COMP_OK for "made progress", so the terminal state has to be
        // checked explicitly (the stateless API would report this as
        // STATELESS_OVERFLOW instead). Not expected to trigger, since io_buf
        // is twice the size of data_buf; failing here makes CompressAndWrite
        // recompress the whole buffer with zlib rather than write a truncated
        // member.
        ret = 1;
      }
      EndCompressIGZIP(isal_strm);
      gz->path = IGZIP;
    }
    in_call = false;
#endif
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
  bool igzip_available = false;

#ifdef USE_IAA
  iaa_available =
      configs[USE_IAA_UNCOMPRESS] &&
      SupportedOptionsIAA(kWindowBitsGzip, *input_length, *output_length) &&
      IsIAADecompressible(input, *input_length, kWindowBitsGzip);
#endif
#ifdef USE_QAT
  qat_available = configs[USE_QAT_UNCOMPRESS] &&
                  SupportedOptionsQAT(kWindowBitsGzip, *input_length);
#endif
#ifdef USE_IGZIP
  igzip_available = configs[USE_IGZIP_UNCOMPRESS];
#endif

  ExecutionPath path_selected = ZLIB;
  if (qat_available) {
    path_selected = QAT;
  } else if (iaa_available) {
    path_selected = IAA;
  } else if (igzip_available) {
    path_selected = IGZIP;
  }

  if (path_selected == IAA) {
#ifdef USE_IAA
    in_call = true;
    ret =
        UncompressIAA(input, input_length, output, output_length,
                      qpl_path_hardware, kWindowBitsGzip, end_of_stream, true);
    gz->path = IAA;
    in_call = false;
#endif  // USE_IAA
  } else if (path_selected == QAT) {
#ifdef USE_QAT
    in_call = true;
    ret = UncompressQAT(input, input_length, output, output_length,
                        kWindowBitsGzip, end_of_stream, true);
    gz->path = QAT;
    in_call = false;
#endif  // USE_QAT
  } else if (path_selected == IGZIP) {
#ifdef USE_IGZIP
    in_call = true;
    struct inflate_state* isal_strm = InitUncompressIGZIP(kWindowBitsGzip);
    if (isal_strm == nullptr) {
      ret = 1;
    } else {
      unsigned long total_in = 0;
      unsigned long total_out = 0;
      ret =
          UncompressIGZIP(isal_strm, input, input_length, output, output_length,
                          &total_in, &total_out, end_of_stream);
      EndUncompressIGZIP(isal_strm);
      gz->path = IGZIP;
    }
    in_call = false;
#endif
  }
  return ret;
}

// pinned says the file reaches zlib because the request was never offloadable
// -- level 0, or a transparent write -- rather than because zlib compression
// was selected. Such a write must not depend on use_zlib_compress being set, or
// the pin turns into a failed write; deflate() carries the same term at its own
// zlib fall-through.
static int GzwriteZlibCompress(gzFile file, voidpc buf, unsigned len,
                               bool pinned) {
  int ret = 0;
  if ((configs[USE_ZLIB_COMPRESS] || pinned) && orig_gzwrite != nullptr) {
    ret = orig_gzwrite(file, buf, len);
  } else {
    ret = 0;
  }
  return ret;
}

static int GzreadZlibUncompress(gzFile file, voidp buf, unsigned len) {
  int ret = 0;
  if (configs[USE_ZLIB_UNCOMPRESS] && orig_gzread != nullptr) {
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
  Log(LogLevel::LOG_INFO, "CompressAndWrite Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", accelerator return code ", ret, ", input ",
      input_len, ", output ", output_len, "\n");

  if (ret == 0) {
    gz->data_buf_pos = input_len;
  } else {
    gz->deflate_stream.next_in = (Bytef*)(gz->data_buf);
    gz->deflate_stream.avail_in =
        static_cast<unsigned int>(gz->data_buf_content);
    gz->deflate_stream.next_out = (Bytef*)(gz->io_buf);
    gz->deflate_stream.avail_out = static_cast<unsigned int>(gz->io_buf_size);
    ret = orig_deflate(&gz->deflate_stream, Z_FINISH);
    Log(LogLevel::LOG_INFO, "CompressAndWrite Line ", __LINE__, ", file ",
        static_cast<void*>(file), ", zlib return code ", ret, ", input ",
        input_len, ", output ", output_len, ", avail_in ",
        gz->deflate_stream.avail_in, ", avail_out ",
        gz->deflate_stream.avail_out, "\n");
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
    Log(LogLevel::LOG_INFO, "CompressAndWrite Line ", __LINE__, ", file ",
        static_cast<void*>(file), ", written to file ", write_ret, "\n");
    if (write_ret >= 0) {
      output_len -= write_ret;
    }
  } while (output_len > 0 && write_ret >= 0);

  if (write_ret == -1) {
    return 1;
  }

  return 0;
}

// Compress and write out everything data_buf holds, leaving the buffer ready
// for more input. The gzwrite loop needs this, and so does every entry point
// that has to make the bytes written so far visible in the file before it acts
// (gzclose, gzflush, gzsetparams, and the handoff to zlib in gzvprintf).
static int FlushBufferedWrite(gzFile file, GzipFile* gz) {
  if (gz->data_buf_content == 0) {
    return 0;
  }

  // CompressAndWrite may fall back to zlib, so it needs the same symbols
  // gzwrite checks for; without them the buffered data cannot be flushed and
  // the file would be silently truncated, so report the failure.
  if (orig_deflate == nullptr || orig_deflateReset == nullptr) {
    Log(LogLevel::LOG_ERROR, "FlushBufferedWrite Line ", __LINE__,
        " a required zlib symbol is unresolved, cannot flush\n");
    return 1;
  }

  int ret = CompressAndWrite(file, gz);
  if (ret != 0) {
    return ret;
  }

  // Shift whatever CompressAndWrite did not consume to the beginning.
  // TODO replace with circular buffer to avoid copy
  uint32_t data_remaining = gz->data_buf_content - gz->data_buf_pos;
  memmove(gz->data_buf, gz->data_buf + gz->data_buf_pos, data_remaining);
  gz->data_buf_content = data_remaining;
  gz->data_buf_pos = 0;
  return 0;
}

int ZEXPORT gzwrite(gzFile file, voidpc buf, unsigned len) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr) {
    return orig_gzwrite != nullptr ? orig_gzwrite(file, buf, len) : 0;
  }

  // Same reasoning as gzread: validate the whole set up front rather than
  // partway through CompressAndWrite, which by then may have buffered data
  // that has to be either written or dropped. 0 is what zlib returns for a
  // write it could not perform.
  if (orig_gzwrite == nullptr || orig_deflate == nullptr ||
      orig_deflateReset == nullptr || orig_deflateInit2_ == nullptr) {
    Log(LogLevel::LOG_ERROR, "gzwrite Line ", __LINE__,
        " a required zlib symbol is unresolved, cannot write\n");
    return 0;
  }

  Log(LogLevel::LOG_INFO, "gzwrite Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", buf ", buf, ", len ", len, "\n");

  unsigned int written_bytes = 0;
  bool accelerator_selected = configs[USE_IAA_COMPRESS] ||
                              configs[USE_QAT_COMPRESS] ||
                              configs[USE_IGZIP_COMPRESS];
  if (gz->path != ZLIB && accelerator_selected) {
    // zlib's own limit: the length has to be representable in the int this
    // returns. Refused before anything is buffered, so the next write does not
    // emit data from a call that reported writing none. A file on the zlib path
    // is refused by zlib itself instead, which also latches the error gzerror
    // reports.
    if (len > static_cast<unsigned>(INT_MAX)) {
      return 0;
    }

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
      Log(LogLevel::LOG_INFO, "gzwrite Line ", __LINE__, ", file ",
          static_cast<void*>(file), ", remaining ", data_buf_remaining,
          ", to copy ", data_to_copy, ", written ", written_bytes, "\n");

      // Compress and write the buffer
      if (written_bytes < len) {
        if (FlushBufferedWrite(file, gz.get()) != 0) {
          written_bytes = 0;
          goto gzwrite_end;
        }
      }
    }
  } else {
    written_bytes = GzwriteZlibCompress(file, buf, len, gz->path == ZLIB);
    gz->path = ZLIB;
  }

gzwrite_end:
  Log(LogLevel::LOG_INFO, "gzwrite Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", written ", written_bytes, ", buffered ",
      gz->data_buf_pos, ", path ", static_cast<int>(gz->path), "\n");

  return written_bytes;
}

// Without interception the level the application asks for here is recorded by
// zlib and honored by nobody: the shim compresses through its own streams, so
// subsequent writes keep the level the file was opened with. Same class of
// silent contract violation deflateParams() had.
int ZEXPORT gzsetparams(gzFile file, int level, int strategy) {
  Log(LogLevel::LOG_INFO, "gzsetparams Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", level ", level, ", strategy ", strategy,
      "\n");
  if (orig_gzsetparams == nullptr) {
    return Z_STREAM_ERROR;
  }

  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzsetparams(file, level, strategy);
  }

  // Forward first: zlib's own checks (write mode, no sticky error, not a
  // transparent file) decide the return value, and zlib has to record the level
  // too, since it is the one that compresses if this file is later handed back.
  const int ret = orig_gzsetparams(file, level, strategy);
  if (ret != Z_OK) {
    return ret;
  }

  // Data still buffered was written under the old level, so it has to go out as
  // a member of its own before the new level is recorded.
  if (FlushBufferedWrite(file, gz.get()) != 0) {
    return Z_STREAM_ERROR;
  }

  gz->level = level;
  // Keep the fallback stream at the level the file now has; it is built by
  // Reset() and outlives any number of members. Reset() builds it with the
  // default strategy and nothing in the shim acts on strategy, so only the
  // level is applied here.
  if (orig_deflateParams != nullptr) {
    orig_deflateParams(&gz->deflate_stream, level, Z_DEFAULT_STRATEGY);
  }

  // Same pin, for the same reason, as the one the constructor applies to a file
  // opened at level 0: no backend can emit stored blocks, and gzwrite only
  // reaches zlib through path == ZLIB.
  if (!IsOffloadableCompressionLevel(level)) {
    gz->path = ZLIB;
  }

  return Z_OK;
}

static bool GzIsWriteMode(FileMode mode) {
  return mode == FileMode::WRITE || mode == FileMode::APPEND;
}

// Forwarding this to zlib would corrupt the file: zlib's gzflush compresses and
// flushes its own deflate stream, which has never seen a byte of a file the
// shim writes, so it emits a gzip header (or a whole empty member) in the
// middle of the members the shim already wrote. What the caller is owed is that
// everything written so far is in the file, and every buffer the shim writes
// out is a complete member, which satisfies that for any flush level zlib
// defines.
int ZEXPORT gzflush(gzFile file, int flush) {
  Log(LogLevel::LOG_INFO, "gzflush Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", flush ", flush, "\n");

  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzflush != nullptr ? orig_gzflush(file, flush) : Z_STREAM_ERROR;
  }

  // zlib's own checks, in zlib's order: a write-mode file and a flush value in
  // range. Z_NO_FLUSH is in range and asks only that pending input be
  // compressed, which is what the flush below does.
  if (!GzIsWriteMode(gz->mode) || flush < 0 || flush > Z_FINISH) {
    return Z_STREAM_ERROR;
  }

  if (FlushBufferedWrite(file, gz.get()) != 0) {
    return Z_ERRNO;
  }
  return Z_OK;
}

// The four helpers below are write-through paths in zlib, so on a file the shim
// owns they have to reach the shim's gzwrite rather than zlib's stream; a
// forwarded call writes a second, interleaved member and the file decompresses
// with the bytes out of order.
int ZEXPORT gzputc(gzFile file, int c) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzputc != nullptr ? orig_gzputc(file, c) : -1;
  }
  if (!GzIsWriteMode(gz->mode)) {
    return -1;
  }

  unsigned char ch = static_cast<unsigned char>(c);
  return gzwrite(file, &ch, 1) == 1 ? static_cast<int>(ch) : -1;
}

int ZEXPORT gzputs(gzFile file, const char* s) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzputs != nullptr ? orig_gzputs(file, s) : -1;
  }
  if (!GzIsWriteMode(gz->mode) || s == nullptr) {
    return -1;
  }

  size_t len = strlen(s);
  // zlib's own limit: the length has to be representable in the int it returns.
  if (len > static_cast<size_t>(INT_MAX)) {
    return -1;
  }
  int written = gzwrite(file, s, static_cast<unsigned>(len));
  return written < static_cast<int>(len) ? -1 : written;
}

z_size_t ZEXPORT gzfwrite(voidpc buf, z_size_t size, z_size_t nitems,
                          gzFile file) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzfwrite != nullptr ? orig_gzfwrite(buf, size, nitems, file)
                                    : 0;
  }
  if (!GzIsWriteMode(gz->mode)) {
    return 0;
  }

  // zlib's overflow check, and its answer of whole items written.
  z_size_t len = nitems * size;
  if (size != 0 && len / size != nitems) {
    return 0;
  }
  z_size_t written = 0;
  while (written < len) {
    z_size_t remaining = len - written;
    // INT_MAX, not UINT_MAX: gzwrite answers in an int, so a larger chunk it
    // wrote in full could only report a negative count, which the check below
    // reads as a failure -- ending the loop with zero items after every byte
    // had reached the file. zlib refuses such a length outright, for the same
    // reason.
    const z_size_t max_chunk = static_cast<z_size_t>(INT_MAX);
    unsigned chunk = remaining > max_chunk ? static_cast<unsigned>(max_chunk)
                                           : static_cast<unsigned>(remaining);
    int ret = gzwrite(file, static_cast<const char*>(buf) + written, chunk);
    if (ret <= 0) {
      break;
    }
    written += static_cast<unsigned>(ret);
  }
  return size != 0 ? written / size : 0;
}

int ZEXPORTVA gzvprintf(gzFile file, const char* format, va_list va) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzvprintf != nullptr ? orig_gzvprintf(file, format, va)
                                     : Z_STREAM_ERROR;
  }
  if (!GzIsWriteMode(gz->mode) || format == nullptr) {
    return Z_STREAM_ERROR;
  }

  // Format here and write the result through gzwrite, rather than flushing and
  // handing the file to zlib: a handoff would put the rest of the file on the
  // zlib path for the sake of one formatted string. The first pass only
  // measures the result, so it needs its own copy of the argument list.
  va_list va_measure;
  va_copy(va_measure, va);
  int len = std::vsnprintf(nullptr, 0, format, va_measure);
  va_end(va_measure);
  if (len < 0) {
    return Z_STREAM_ERROR;
  }
  if (len == 0) {
    return 0;
  }

  std::string formatted;
  try {
    // vsnprintf writes a terminator past the formatted bytes.
    formatted.resize(static_cast<size_t>(len) + 1);
  } catch (...) {
    return Z_MEM_ERROR;
  }
  std::vsnprintf(&formatted[0], formatted.size(), format, va);
  if (gzwrite(file, formatted.data(), static_cast<unsigned>(len)) != len) {
    return Z_ERRNO;
  }
  return len;
}

int ZEXPORTVA gzprintf(gzFile file, const char* format, ...) {
  va_list va;
  va_start(va, format);
  int ret = gzvprintf(file, format, va);
  va_end(va);
  return ret;
}

static int GzreadOwnedFile(gzFile file, GzipFile* gz, voidp buf, unsigned len) {
  // Check every symbol this function may need before touching any state. The
  // accelerator path can hand the rest of the file to zlib at any point, and a
  // concatenated stream needs a reset between members, so a check made partway
  // through would have to either abandon bytes already copied into the
  // caller's buffer or continue without making progress. Fail fast instead.
  // orig_inflateInit2_ matters because GzipFile::Reset only initializes
  // inflate_stream when it resolved; without it the fallback would inflate on
  // a zeroed stream.
  if (orig_gzread == nullptr || orig_inflate == nullptr ||
      orig_inflateReset == nullptr || orig_inflateInit2_ == nullptr) {
    Log(LogLevel::LOG_ERROR, "gzread Line ", __LINE__,
        " a required zlib symbol is unresolved, cannot read\n");
    return -1;
  }

  Log(LogLevel::LOG_INFO, "gzread Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", buf ", buf, ", len ", len, "\n");

  int ret = 1;
  uint32_t read_bytes = 0;
  bool accelerator_selected = configs[USE_IAA_UNCOMPRESS] ||
                              configs[USE_QAT_UNCOMPRESS] ||
                              configs[USE_IGZIP_UNCOMPRESS];
  if (gz->path != ZLIB && accelerator_selected) {
    // zlib's own limit, as in gzwrite: the length has to be representable in
    // the int this returns, and a file on the zlib path is refused by zlib.
    if (len > static_cast<unsigned>(INT_MAX)) {
      return -1;
    }

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
      Log(LogLevel::LOG_INFO, "gzread Line ", __LINE__, ", file ",
          static_cast<void*>(file), ", remaining ", data_remaining,
          ", to copy ", data_to_copy, ", read ", read_bytes, "\n");

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
            Log(LogLevel::LOG_INFO, "gzread Line ", __LINE__, ", file ",
                static_cast<void*>(file), ", read from file ", read_ret, "\n");
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
            Log(LogLevel::LOG_INFO, "gzread Line ", __LINE__, ", file ",
                static_cast<void*>(file), ", accelerator return code ", ret,
                ", input ", input_len, ", output ", output_len, "\n");

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
            Log(LogLevel::LOG_INFO, "gzread Line ", __LINE__, ", file ",
                static_cast<void*>(file), ", zlib return code ", ret,
                ", input ", input_len, ", output ", output_len, ", avail_in ",
                gz->inflate_stream.avail_in, ", avail_out ",
                gz->inflate_stream.avail_out, "\n");
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
  Log(LogLevel::LOG_INFO, "gzread Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", return code ", ret, ", read ", read_bytes,
      ", buffered compressed ", gz->io_buf_content, ", buffered uncompressed ",
      gz->data_buf_content - gz->data_buf_pos, ", path ",
      static_cast<int>(gz->path), "\n");
  return read_bytes;
}

int ZEXPORT gzread(gzFile file, voidp buf, unsigned len) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr) {
    return orig_gzread != nullptr ? orig_gzread(file, buf, len) : -1;
  }

  // A byte pushed back by gzungetc is the first thing the next read returns;
  // the rest of the request comes from the file as usual.
  if (gz->pushback >= 0 && len > 0 && buf != nullptr) {
    *static_cast<char*>(buf) = static_cast<char>(gz->pushback);
    gz->pushback = -1;
    if (len == 1) {
      return 1;
    }
    int ret =
        GzreadOwnedFile(file, gz.get(), static_cast<char*>(buf) + 1, len - 1);
    // The pushed byte reached the caller even if the rest of the read did not.
    return ret > 0 ? ret + 1 : 1;
  }

  return GzreadOwnedFile(file, gz.get(), buf, len);
}

// The read counterparts of the write helpers: each reads through zlib's own gz
// stream, which on a file the shim owns is positioned nowhere near where the
// shim has read to, so a forwarded call returns bytes from the middle of a
// compressed member. Served here through the shim's own gzread instead.
//
// zlib.h makes gzgetc a macro that serves a byte straight out of zlib's buffer
// and only calls the function when that buffer is empty -- which, on a file the
// shim reads, it always is. The definition here therefore has to shed the macro
// first, exactly as zlib's own gzread.c does.
#undef gzgetc
int ZEXPORT gzgetc(gzFile file) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzgetc != nullptr ? orig_gzgetc(file) : -1;
  }
  if (gz->mode != FileMode::READ) {
    return -1;
  }

  unsigned char ch = 0;
  return gzread(file, &ch, 1) == 1 ? static_cast<int>(ch) : -1;
}

// zlib exports both the macro above and this plain function, for callers that
// want the symbol rather than the macro.
int ZEXPORT gzgetc_(gzFile file) { return gzgetc(file); }

int ZEXPORT gzungetc(int c, gzFile file) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzungetc != nullptr ? orig_gzungetc(c, file) : -1;
  }
  // zlib's own refusals: a file that is not being read, a byte that is not one,
  // and a push onto a byte that has not been read back yet.
  if (gz->mode != FileMode::READ || c < 0 || gz->pushback >= 0) {
    return -1;
  }

  // Only the pushback byte changes. reached_eof records that the file itself
  // has been read to its end, which is still true and is what stops gzread from
  // reading it again; gzeof is the one that has to account for this byte, and
  // it does so by looking at the pushback rather than at that flag.
  gz->pushback = static_cast<int>(static_cast<unsigned char>(c));
  return gz->pushback;
}

char* ZEXPORT gzgets(gzFile file, char* buf, int len) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzgets != nullptr ? orig_gzgets(file, buf, len) : nullptr;
  }
  if (buf == nullptr || len < 1 || gz->mode != FileMode::READ) {
    return nullptr;
  }

  // Up to a newline, which is kept, or len - 1 bytes, whichever comes first.
  int copied = 0;
  while (copied < len - 1) {
    int c = gzgetc(file);
    if (c < 0) {
      break;
    }
    buf[copied++] = static_cast<char>(c);
    if (c == '\n') {
      break;
    }
  }
  // Nothing read at all means end of file, which zlib reports as no string
  // rather than an empty one.
  if (copied == 0) {
    return nullptr;
  }
  buf[copied] = '\0';
  return buf;
}

z_size_t ZEXPORT gzfread(voidp buf, z_size_t size, z_size_t nitems,
                         gzFile file) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzfread != nullptr ? orig_gzfread(buf, size, nitems, file) : 0;
  }
  if (gz->mode != FileMode::READ) {
    return 0;
  }

  // zlib's overflow check, and its answer of whole items read.
  z_size_t len = nitems * size;
  if (size != 0 && len / size != nitems) {
    return 0;
  }
  z_size_t read_total = 0;
  while (read_total < len) {
    z_size_t remaining = len - read_total;
    // INT_MAX, not UINT_MAX, for the same reason as gzfwrite: gzread answers in
    // an int and refuses anything larger.
    const z_size_t max_chunk = static_cast<z_size_t>(INT_MAX);
    unsigned chunk = remaining > max_chunk ? static_cast<unsigned>(max_chunk)
                                           : static_cast<unsigned>(remaining);
    int ret = gzread(file, static_cast<char*>(buf) + read_total, chunk);
    if (ret <= 0) {
      break;
    }
    read_total += static_cast<unsigned>(ret);
  }
  return size != 0 ? read_total / size : 0;
}

// gzclose, gzclose_r and gzclose_w share one body: whichever entry point the
// application used, a file the shim owns needs its buffered data flushed, its
// size captured before zlib's own close appends to it, and that appended data
// truncated away again.
//
// required_mode is what separates the three, and it has to be checked before
// any of that work happens. zlib's gzclose_r and gzclose_w reject a file opened
// for the other direction without touching it, so a mismatched call here must
// not flush or truncate a file zlib would have left alone. FileMode::NONE
// accepts either direction and is what gzclose passes. Append is checked as a
// write because zlib folds it into its own write mode right after opening the
// file, which is why zlib's gzclose_w accepts an appended file.
static bool GzCloseModeMatches(FileMode mode, FileMode required_mode) {
  switch (required_mode) {
    case FileMode::READ:
      return mode == FileMode::READ;
    case FileMode::WRITE:
    case FileMode::APPEND:
      return mode == FileMode::WRITE || mode == FileMode::APPEND;
    case FileMode::NONE:
      break;
  }
  return true;
}

static int GzCloseCommon(gzFile file, FileMode required_mode,
                         int (*orig_close)(gzFile file)) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr || orig_close == nullptr) {
    // Every path below ends in orig_close, so without it there is nothing
    // useful to do; leave the entry in place rather than losing the state of a
    // file that stays open. Z_STREAM_ERROR matches what zlib's own close
    // functions return for a file they cannot act on.
    return orig_close != nullptr ? orig_close(file) : Z_STREAM_ERROR;
  }

  if (!GzCloseModeMatches(gz->mode, required_mode)) {
    return Z_STREAM_ERROR;
  }

  // Unregister up front, before orig_close frees the gzFile. Unsetting after
  // the free would erase the entry of whatever file has since been allocated at
  // the same address, so do it once here rather than on each exit path. Holding
  // gz (a shared_ptr) keeps this file's state alive until we return.
  gzip_files.Unset(file);

  Log(LogLevel::LOG_INFO, "GzCloseCommon Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", buffered ", gz->data_buf_content, ", path ",
      static_cast<int>(gz->path), "\n");

  int ret = 0;
  if (gz->path != ZLIB &&
      (gz->mode == FileMode::WRITE || gz->mode == FileMode::APPEND)) {
    // Compress any remaining buffered data.
    int write_ret = FlushBufferedWrite(file, gz.get());

    // Capture file size and name before the close
    off_t file_size = lseek(gz->fd, 0, SEEK_CUR);
    char file_path[MAXPATHLEN];
    ssize_t readlink_ret =
        readlink(("/proc/self/fd/" + std::to_string(gz->fd)).c_str(), file_path,
                 MAXPATHLEN - 1);
    if (readlink_ret == -1) {
      ret = orig_close(file);
      Log(LogLevel::LOG_ERROR, "GzCloseCommon Line ", __LINE__,
          ", readlink_ret return error \n");
      return ret;
    }
    file_path[readlink_ret] = '\0';

    int close_ret = orig_close(file);

    // Remove any file content added by the close
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
    ret = orig_close(file);
  }
  Log(LogLevel::LOG_INFO, "GzCloseCommon Line ", __LINE__, ", file ",
      static_cast<void*>(file), ", return code ", ret, ", buffered processed ",
      gz->data_buf_pos, "\n");
  return ret;
}

int ZEXPORT gzclose(gzFile file) {
  return GzCloseCommon(file, FileMode::NONE, orig_gzclose);
}

// Without these two, a file the shim owns and the application closes through
// gzclose_r/gzclose_w never reaches GzCloseCommon: the buffered data is not
// flushed, the registry entry outlives the gzFile, and the content zlib's own
// close appends is left in the file instead of being truncated away.
int ZEXPORT gzclose_r(gzFile file) {
  return GzCloseCommon(file, FileMode::READ, orig_gzclose_r);
}

int ZEXPORT gzclose_w(gzFile file) {
  return GzCloseCommon(file, FileMode::WRITE, orig_gzclose_w);
}

int ZEXPORT gzeof(gzFile file) {
  auto gz = gzip_files.Get(file);
  // reached_eof is only maintained by the accelerator read path in gzread. Once
  // a file is on the zlib path, zlib owns its end-of-file state, so ask zlib
  // rather than reporting a flag that will never be set.
  if (gz == nullptr || gz->path == ZLIB) {
    return orig_gzeof != nullptr ? orig_gzeof(file) : 0;
  }
  // reached_eof only records that a read of the file came up short; the buffers
  // may still hold data gzread has not handed back yet. Reporting EOF here
  // would silently truncate a "while (!gzeof(file)) gzread(...)" loop, so match
  // gzread's own view of whether more data is available (see the
  // file_data_remaining/data_remaining checks in its loop above).
  // A byte pushed back by gzungetc counts as data too: it is the next thing a
  // read returns.
  bool data_remaining = (gz->data_buf_content - gz->data_buf_pos) > 0 ||
                        (gz->io_buf_content - gz->io_buf_pos) > 0 ||
                        gz->pushback >= 0;
  return gz->reached_eof && !data_remaining;
}

ExecutionPath GetGzipFileExecutionPath(gzFile file) {
  auto gz = gzip_files.Get(file);
  if (gz == nullptr) {
    return ZLIB;
  }
  return gz->path;
}
#if defined(__clang__)
#pragma clang attribute pop
#endif
