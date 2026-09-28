# zlib-accel: Transparent Compression/Decompression Acceleration for Zlib

zlib-accel is a software shim that intercepts zlib calls and offloads compression/decompression jobs to a hardware accelerator or to a SIMD software encoder, where the request is one the backend can take.
The shim allows applications to use those backends without code changes: preload the shim's shared library (e.g., using LD_PRELOAD), and enable a backend in the configuration file. Applications that depend on one of the zlib behaviors listed below may additionally need to disable offload for the streams concerned.

Three backends are supported
- Intel® QuickAssist Technology (QAT), a hardware accelerator
- Intel® In-Memory Analytics Accelerator (IAA), a hardware accelerator
- IGZIP, the SIMD deflate implementation in [Intel® ISA-L](https://github.com/intel/isa-l). Software only, so it requires no accelerator hardware.


## Scope/Constraints

This shim is not a general-purpose replacement for zlib: it offloads only the jobs a backend can actually take, and falls back to zlib for everything else, so for most applications the observable behavior is zlib's own. In general that means completing a whole deflate stream in one call — "streaming" (incremental) compression/decompression is offloadable through IGZIP only, since the two hardware accelerators don't support it. It is not a blanket guarantee: an offloaded stream diverges from zlib in specific, documented ways, and each divergence says what an application that depends on it should do. Test thoroughly with your own application and configuration; tested use cases are listed below.

See **[docs/constraints.md](docs/constraints.md)** for the per-backend limits (buffer/window sizes, streaming support) and the zlib behaviors offload doesn't preserve (`strategy`, compression-level mapping, `adler`/`data_type`), and [Intercepted Zlib Functions](#intercepted-zlib-functions) below for which entry points those affect and why.

The shim has only been tested on Linux. When building with hardware acceleration enabled (`USE_QAT`, `USE_IAA`, or `USE_IGZIP`), the [Intel® oneTBB](https://www.intel.com/content/www/us/en/developer/tools/oneapi/onetbb.html) performance library is required and can be acquired by installing the [Intel® oneAPI Base Toolkit](https://www.intel.com/content/www/us/en/developer/tools/oneapi/oneapi-toolkit-download.html).

CI for HW offload tests is in development (tests are currently run internally).


## Releases

Tagged releases are the supported way to consume the project, and each release's notes state what it contains, which accelerators it covers, and any change to the build requirements or the configuration options. Commits on the main branch that are not tagged as releases are not to be considered stable.


## Build the Shared Library

```
mkdir build
cd build
cmake <options> ..
make
```

CMake supports the following options. A backend has to be enabled at build time *and* at run time, so building with `USE_IGZIP=ON` is not on its own enough to get IGZIP — see the configuration section.
- USE_QAT (ON/OFF, default OFF): include QAT acceleration
- USE_IAA (ON/OFF, default OFF): include IAA acceleration
- USE_IGZIP (ON/OFF, default OFF): include IGZIP acceleration (requires ISA-L v2.32.1 or above; the build fails at configure time on an older one)
- QPL_PATH: path to QPL for IAA acceleration (if not in a standard directory)
- QATZIP_PATH: path to QATzip for QAT acceleration (if not in a standard directory)
- ISAL_PATH: path to ISA-L for IGZIP acceleration (if not in a standard directory). May be either an ISA-L build tree or an install prefix.
- DEBUG_LOG (ON/OFF, default ON): enable logging
- ENABLE_STATISTICS (ON/OFF, default OFF): enable statistics
- COVERAGE (ON/OFF, default OFF): enable test coverage (more details in a later section)
- CMAKE_BUILD_TYPE (Debug/Release...)

For a release build, the following options are recommended: 

```
-DDEBUG_LOG=OFF -DCOVERAGE=OFF -DCMAKE_BUILD_TYPE=Release
```

Requirements for QAT
- QAT hardware (integrated in 4th Gen Intel Scalable Processor and later)
- QAT drivers, available in-tree in Linux kernel
- [QATlib](https://github.com/intel/qatlib) library
- [QATzip](https://github.com/intel/qatzip) library (v1.3.0 and above)

Requirements for IAA
- IAA hardware (integrated in 4th Gen Intel Scalable Processor and later)
- idxd driver, available in-tree in Linux kernel
- [accel-config](https://github.com/intel/idxd-config)
- [Query Processing Library](https://github.com/intel/qpl)

Requirements for IGZIP
- [ISA-L (Intel Intelligent Storage Acceleration Library)](https://github.com/intel/isa-l) (v2.32.1 and above). Earlier versions contain igzip defects that zlib-accel no longer works around, so the build refuses them; distribution packages are often older than this (Ubuntu 22.04 ships 2.30.x), in which case ISA-L has to be built from source and pointed at with `ISAL_PATH`.

A setup with both QAT and IAA enabled has been tested on an AWS m7i.metal-24xl instance (Ubuntu 22.04, kernel 6.8.0).
Refer to the links above for instructions on how to install the dependencies.


### Format Code

To format the code, execute the "format" target from the build directory:

```
make format
```


## Build and Run Tests

GoogleTest is required to build the tests.

```
cd tests
mkdir build
cd build
cmake <options> ..
make
make run
```

The CMake options are the same as for the shared library build, except for ENABLE_STATISTICS, which is declared by the library build only. The tests must be configured with the same USE_QAT/USE_IAA/USE_IGZIP values as the library they link against.


### Collect Test Coverage

- Build the shared library and tests with COVERAGE=ON in CMake commands.
- Follow the instructions to run the tests.
- After running the tests, generate the HTML report with

```
make coverage
```


### Fuzz Testing

```
cd fuzzing
mkdir build
cd build
cmake <options> ..
make
make run
```

The CMake options are the same as for the shared library build, except for ENABLE_STATISTICS, which is declared by the library build only. Clang is required.
For libFuzzer command-line options, refer to the [documentation](https://llvm.org/docs/LibFuzzer.html).


### Sanitizers

Sanitizers can be enabled for library and test builds
- ASAN (ON/OFF): AddressSanitizer
- UBSAN (ON/OFF): UndefinedBehaviorSanitizer
- TSAN (ON/OFF): ThreadSanitizer. It may require disabling ASLR at runtime.


## Configuration

The shim is configured through a file at /etc/zlib-accel.conf
The following options are supported.

use_qat_compress
- Values: 0,1. Default: 1
- Enable QAT for compression

use_qat_uncompress
- Values: 0,1. Default: 1
- Enable QAT for decompression

use_iaa_compress
- Values: 0,1. Default: 0
- Enable IAA for compression

use_iaa_uncompress
- Values: 0,1. Default: 0
- Enable IAA for decompression

use_zlib_compress
- Values: 0,1. Default: 1
- Enable zlib for compression
- Setting to 1 is recommended, to allow fall back to zlib in case accelerators cannot be used or experience an error.

use_zlib_uncompress
- Values: 0,1. Default: 1
- Enable zlib for decompression
- Setting to 1 is recommended, to allow fall back to zlib in case accelerators cannot be used or experience an error.

use_igzip_compress
- Values: 0,1. Default: 0
- Enable IGZIP for compression

use_igzip_uncompress
- Values: 0,1. Default: 0
- Enable IGZIP for decompression
- IGZIP requires no accelerator hardware, but it is not used unless one of these two options is set, even in a build configured with USE_IGZIP=ON.

igzip_fallback
- Values: 0,1. Default: 1
- If 1, and an IAA or QAT operation fails inside deflate() or inflate(), the request is retried using IGZIP, if IGZIP is enabled for that direction, before falling back to software zlib. Useful on machines where hardware accelerators are intermittently unavailable.
- This option does not apply to compress2()/uncompress2(), which fall back directly to zlib.

iaa_compress_percentage
- Values: 0-100. Default: 50
- If both IAA and QAT are enabled, percentage of compression calls to offload to IAA.

iaa_prepend_empty_block
- Values: 0,1. Default: 0
- **Deprecated.** This option is retained for backward compatibility and will be removed in a future release. If 1, an empty stored block is prepended to IAA-compressed output; nothing reads it back, so it has no effect on decompression. IAA decompression eligibility is decided by a minimum input length instead — see the comment in `iaa.cpp` for why the marker scheme was replaced.

iaa_uncompress_percentage
- Values: 0-100. Default: 50
- If both IAA and QAT are enabled, percentage of decompression calls to offload to IAA.

qat_periodical_polling
- Values: 0,1. Default: 0
- If 1, use QAT periodical polling. If 0, use QAT busy polling.

qat_compression_level
- Values: 1,9. Default: 1
- QAT compression level

qat_compression_allow_chunking
- Values: 0,1. Default: 0
- If set to 1, data larger than the QAT HW buffer (512kB) will be split into chunks of HW buffer size when compressing with QAT. This causes the compressed data to be a concatenation of multiple streams (one per chunk). This is not the same behavior as for zlib, which creates a single stream. If decompression expects a single stream, this may cause issues.
- If set to 0, this option disables chunking for QAT compression. If the input data is larger than the QAT HW buffer, QAT will not be used. This improves zlib compatibility, but it may reduce QAT utilization depending on the workload.

ignore_zlib_dictionary
- Values: 0, 1. Default: 0
- If set to 1, zlib-accel ignores deflateSetDictionary and inflateSetDictionary.
- If set to 0, zlib-accel honors inflateSetDictionary and deflateSetDictionary.

log_level
- Values: 0,1,2,3. Default: 1
- This option applies only if the shim is built with DEBUG_LOG=ON.
- Matches QATzip's verbosity convention: 0 = silent, 1 = errors only, 2 = info and errors, 3 = debug, info, and errors (most verbose).

log_stats_samples
- Values: 0-UINT32_MAX. Default 1000
- This option applies only if the shim is built with ENABLE_STATISTICS=ON.
- Append statistics to log every N samples (this option specifies N). A sample is one deflate() or inflate() call.
- If set to 0, statistics are not appended to the log.

log_file
- Values: path. Default: /tmp/zlib-accel.log
- This option applies only if the shim is built with DEBUG_LOG=ON or ENABLE_STATISTICS=ON.
- If specified, store log messages in the file. If not specified, log messages are printed to stdout.

map_shards
- Values: 2-65536. Default 64
- Sets the number of shards in the thread-safe concurrent hash map. Each shard holds an independent map instance.
- It must be a power of two.

## Tested Applications/Use Cases

### RocksDB

Tested with db_bench benchmarking tool

```
LD_PRELOAD=<path_to_shim> db_bench --compression_type=zlib <other_options>
```

### Postgres Backup

Tested with gzip-compressed backup using pg_dump and pg_restore.

```
export LD_PRELOAD=<path_to_shim>
pg_dump <db_name> --create --format=directory --compress=gzip <other_options>
pg_restore --clean --create <other_options>
unset LD_PRELOAD
```

## Intercepted Zlib Functions

deflate/inflate and related functions
- deflateInit, deflateInit2, deflateSetDictionary, deflateParams, deflatePrime, deflateCopy, deflate, deflateEnd, deflateReset, deflateResetKeep
- inflateInit, inflateInit2, inflateSetDictionary, inflatePrime, inflateCopy, inflate, inflateEnd, inflateReset, inflateReset2, inflateResetKeep, inflateSync

An offloaded call never advances zlib's own state for that stream, so zlib cannot take over part-way through, and a zlib call that answered from that state would describe an empty stream. That drives every divergence below.

- **Flush values**: QAT/IAA take only a single `Z_FINISH` call carrying the whole input; everything else runs on zlib. IGZIP is stateful and also takes `Z_NO_FLUSH`/`Z_SYNC_FLUSH`/`Z_PARTIAL_FLUSH`/`Z_FULL_FLUSH` (`Z_BLOCK` is the exception noted in docs/constraints.md), staying on it for the life of the stream. A no-input flush that doesn't outrank the previous flush is refused with `Z_BUF_ERROR`, matching zlib's own repeated-flush rule, so a timer-driven flush with no new data doesn't grow the stream.
- **Dictionaries**: `deflateSetDictionary`/`inflateSetDictionary` pin the stream to zlib — no backend supports preset dictionaries. A zlib-header FDICT bit is detected before dispatch and pins the stream too, even off a single header byte, since an accelerator that consumed that byte couldn't hand it back to zlib.
- **`deflateParams`**: always forwarded to zlib; also refreshes the recorded level for path selection and drops an IGZIP stream built for a level the call supersedes.
- **`inflateReset2`**: the only entry point that changes `windowBits` on a live stream, so it refreshes the recorded window/format, discards (rather than resets) any IGZIP decompression state built for the old one, and clears the terminal-state gate below.
- **Terminal-state gate**: once a stream returns `Z_STREAM_END`, later `deflate`/`inflate` calls are answered from that gate — matching zlib's return code and `msg` behavior, consuming no input and writing no output — instead of being dispatched to an engine, which would otherwise append a second stream onto a finished one. `*Reset*` clears the gate; `*Copy` carries it to the copy. Two other entry points still diverge on a finished stream: `deflateParams` returns `Z_OK` where zlib errors, and `deflateSetDictionary` returns `Z_STREAM_ERROR` where zlib succeeds — applications that inspect these codes on a finished stream should disable offload for it.
- **`*ResetKeep`**: intercepted so a terminal state can't wedge the stream at `Z_STREAM_END` across a restart. `inflateResetKeep` additionally pins the stream to zlib, since keeping the window means a later stream may reference history no accelerator can see; a later `inflateReset` lifts the pin. The pin only decides which engine runs next — it can't retroactively supply an accelerator's history, so a later stream that actually needs it gets `Z_DATA_ERROR` on zlib rather than an accelerator silently mis-decoding it against the wrong window.
- **Mid-stream decode failure**: `inflate` returns `Z_DATA_ERROR` after handing back whatever it had already decoded, then latches that error on every later call — zlib's own behavior — without handing the stream to zlib to finish, since the remaining input doesn't sit at a boundary zlib could parse. Only a stateful engine (IGZIP, including via `igzip_fallback`) can fail mid-stream; a first-call failure still goes to zlib.
- **`inflateSync`**: the one call that legitimately resumes a stream elsewhere — it searches for a full-flush point in zlib's own state. A stream that has consumed input is pinned to zlib from that point (the same pin `inflateResetKeep` applies), and any latched data error is cleared. A stream that hasn't consumed anything keeps its engine.
- **`*Prime`**: no backend has a bit buffer, so priming at least one bit pins the stream to zlib, the only engine that can honor it; a zero-bit prime, or one zlib itself rejects, leaves the stream offloadable. Once an engine already holds the stream (output produced or input consumed), a prime call zlib would accept is instead refused with `Z_STREAM_ERROR` — a deliberate divergence, in place of bits that would otherwise silently vanish. `*Reset` discards zlib's bit buffer and lifts both the pin and the refusal.
- **`*Copy`**: gives the copy its own per-stream state, since zlib-accel keys state on the `z_stream` pointer and an unhandled copy would silently run on zlib. `inflateCopy` works on every path; `deflateCopy` has one restriction, described in docs/constraints.md.

The deflate/inflate entry points below are **not** intercepted: each runs zlib's own code against zlib's own state for the stream, which for the reason above describes an empty stream rather than the one the application has been using. The calls do no harm; they answer from nothing. None of them affects compressed or decompressed output.

| Not intercepted | Consequence on an offloaded stream |
|---|---|
| `deflateSetHeader` | Recorded in zlib's state only; the backend writes its own default header, so fields set through it (file name, comment, extra field, mtime) don't appear in the output. |
| `inflateGetHeader` | Never filled in by an offloaded stream; fields stay as the caller left them and `done` stays 0. |
| `deflateGetDictionary`, `inflateGetDictionary` | Read zlib's window, which an offloaded stream never wrote to — length comes back 0. A stream that *sets* a dictionary is pinned to zlib and unaffected. |
| `inflateMark`, `inflateCodesUsed` | Report from zlib's inflate state, which no backend exposes — same reason `data_type` is left alone. |
| `deflatePending` | Always 0 for an offloaded stream; no backend has a faithful number to give instead. |
| `inflateValidate` | Can't disable checksum verification on any backend, so a stream with a bad/absent checksum still errors instead of returning `Z_STREAM_END`. |
| `deflateBound`, `compressBound` | Answer from zlib's own formula; no backend guarantees its output fits it, so keep honoring `deflate`'s return code instead of assuming the bound. |

utility functions
- compress, compress2, uncompress, uncompress2

gzip file functions
- gzopen, gzopen64, gzdopen, gzclose, gzclose_r, gzclose_w, gzeof
- gzwrite, gzputc, gzputs, gzfwrite, gzprintf, gzvprintf, gzflush, gzsetparams
- gzread, gzgetc, gzgetc_, gzgets, gzfread, gzungetc
- gztell, gztell64, gzoffset, gzoffset64, gzseek, gzseek64, gzrewind, gzerror, gzclearerr, gzbuffer, gzdirect

zlib's `gz*` API streams a single deflate/inflate state per file across every call; zlib-accel doesn't. Writes are buffered and compressed into complete gzip members — valid gzip, but no match crosses a member boundary, costing some ratio — and reads decompress a member within one internal buffer or fall back to zlib for the rest of the file. Every byte-moving call is therefore intercepted and served from zlib-accel's own buffer instead of zlib's (untouched) stream for the file, which would otherwise interleave or misread data: `gzputc`/`gzputs`/`gzfwrite`/`gzprintf`/`gzvprintf` share `gzwrite`'s path, `gzgetc`/`gzgetc_`/`gzgets`/`gzfread` share `gzread`'s. `gzprintf`/`gzvprintf` format the string themselves rather than handing it to zlib, which also removes zlib's length limit (zlib silently drops output that doesn't fit its buffer; zlib-accel always writes the whole string) — a deliberate divergence, and why `gzbuffer`'s size isn't actually applied (below).

`gzflush` writes the buffered data as a complete member rather than forwarding to zlib, whose flush would write a header for a stream holding none of this file's data. `gzungetc` pushes back without limit and clears EOF, satisfying zlib's push-back guarantee without needing zlib's buffer size. `gzclose_r`/`gzclose_w` share `gzclose`'s path (flushing whatever's buffered) and reject a file opened for the other direction, as zlib does. The compression level from `gzopen`/`gzdopen`'s mode string or `gzsetparams` reaches path selection and the compressor: level 0 routes to zlib, 1-9 is honored where the backend has a level to set.

A `gzFile` zlib-accel has no entry for (not opened through `gzopen`/`gzdopen`, including a failed open's `NULL`) is forwarded to zlib unchanged.

`gztell`/`gzoffset`/`gzseek`/`gzrewind`/`gzerror`/`gzclearerr` report on zlib-accel's own offsets, descriptor, and error latch instead of zlib's unused state — a backward seek re-reads from the start, and the error latch refuses later calls, matching zlib. `gzopen64` and the `*64` variants of `gzseek`/`gztell`/`gzoffset` are accelerated the same way as their base names, with `_FILE_OFFSET_BITS=64` narrowing refused rather than truncated.

Two answer differently from zlib:

| Intercepted, with a difference | Behavior on a file zlib-accel owns |
|---|---|
| `gzbuffer` | Validated as zlib validates it, then not applied — zlib-accel's buffers are a fixed size, so this is a performance difference, not a correctness one. |
| `gzdirect` | On a read-mode file, answered from zlib-accel's own look at the first bytes rather than zlib's; a write-mode file is answered by zlib, which holds the only state the call reads. |


## Other Notes

### Preload Conflicts

When zlib-accel is preloaded, its dependencies will be preloaded with it. If other libraries require particular versions of certain dependencies to be preloaded as well, there may be precedence issues. In these cases, it is important to specify libraries to preload in the right order.

One example is libcrypto. When zlib-accel is built with QAT support, it is linked to QATlib, which in turn requires libcrypto (for cryptographic functions not used in zlib-accel). As zlib-accel is preloaded, the system libcrypto required by QATlib is loaded as well. If other libraries require particular versions of libcrypto to be preloaded, QATlib loading the system libcrypto first may interfere with that.

One such example is the Amazon Corretto Crypto Provider (ACCP). To avoid compatibility issues, ACCP includes its own copy of libcrypto (refer to the [ACCP readme](https://github.com/corretto/amazon-corretto-crypto-provider/blob/main/README.md#compatibility--requirements)). ACCP tries to load its own libcrypto first (using RPath), but zlib-accel has precedence over it using LD_PRELOAD.

This issue was observed when running certain Cassandra tests with zlib-accel preloaded:
```
ant testsome -Dtest.name=org.apache.cassandra.security.CryptoProviderTest -Dtest.methods=testCryptoProviderInstallation
ant testsome -Dtest.name=org.apache.cassandra.security.CryptoProviderTest -Dtest.methods=testProviderInstallsJustOnce
```
Cassandra tries to load ACCP, but it fails due to the incompatible libcrypto. If Cassandra cannot load ACCP, it will fall back to the Sun provider. That causes these tests to fail.

To work around the issue
- After building Cassandra, find the ACCP jar in the build directory and unpack it (for example, under build/lib/jars)
- In the extracted files, find the ACCP copy of libcrypto.so (for example, under com/amazon/corretto/crypto/provider/libcrypto.so)
- Preload the ACCP libcrypto before zlib-accel (in LD_PRELOAD, list ACCP libcrypto before zlib-accel)
