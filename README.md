# zlib-accel: Transparent Compression/Decompression Acceleration for Zlib

zlib-accel is a software shim that intercepts zlib calls and offloads compression/decompression jobs to hardware accelerators (where feasible and beneficial for performance).
The shim allows applications to leverage hardware accelerators transparently without code changes. The only requirement is to preload the shim's shared library (e.g., using LD_PRELOAD).

Two accelerators are supported
- Intel® QuickAssist Technology (QAT)
- Intel® In-Memory Analytics Accelerator (IAA)


## Scope/Constraints

This shim is not a general-purpose replacement for zlib, and it is able to offload compression/decompression jobs in certain conditions. Therefore, not all applications can take advantage of the transparent offload, depending on how they use zlib. It is important to test thoroughly with your specific application and configuration. The use cases we have tested so far are listed in a section below.

In general, the shim is able to offload zlib calls that complete compression/decompression of one deflate stream in one call. "Streaming" compression/decompression (where compression/decompression is done incrementally) are not currently supported. If the shim is not able to offload a job to an accelerator, it will fall back to zlib, ensuring the application still works correctly.

The shim has only been tested on Linux. When building with hardware acceleration enabled (`USE_QAT`, `USE_IAA`, or `USE_IGZIP`), the [Intel® oneTBB](https://www.intel.com/content/www/us/en/developer/tools/oneapi/onetbb.html) performance library is required and can be acquired by installing the [Intel® oneAPI Base Toolkit](https://www.intel.com/content/www/us/en/developer/tools/oneapi/oneapi-toolkit-download.html).


### Hardware Acceleration

QAT
- Max buffer size (for compressed/uncompressed data): 512kB
- Compression: 
  - Input/output larger than the max buffer size will be compressed into multiple streams (for gzip or zlib formats) or multiple blocks in one stream (for deflate raw format). Note that generating multiple streams is not completely aligned with zlib behavior. This behavior can be controlled using the qat_compression_allow_chunking option.
- Decompression
  - If end-of-stream is not reached in one call, zlib-accel will fall back to zlib. Resuming decompression mid-stream (stateful decompression) is not supported by the accelerator.
  - If the input data contains more than one stream, decompression stops at the first end-of-stream (same as zlib).

IAA
- Max buffer size (for compressed/uncompressed data): 2MB
- History window: 4kB
- Compression:
  - Input/output larger than the max buffer size will be handled by zlib (compression in multiple blocks for larger buffers will be enabled in later releases).
- Decompression:
  - If end-of-stream is not reached in one call, zlib-accel will fall back to zlib (stateful decompression will be enabled in later releases).
  - If the input data contains more than one stream, decompression stops at the first end-of-stream (same as zlib).
  - Data compressed with a history window > 4kB is in general not decompressible with IAA (zlib default window is 32kB).

IGZIP
- ISA-L software SIMD deflate; no hardware accelerator required. Unlike QAT and IAA, it supports genuine streaming (stateful) compression and decompression, so it is also usable as a fallback for the two hardware backends (see the igzip_fallback option).
- Compression:
  - `Z_BLOCK` is not offloadable. It ends a deflate block without byte-aligning the output and without emitting the `00 00 FF FF` sync marker, which ISA-L cannot express — its only two flushing modes, `SYNC_FLUSH` and `FULL_FLUSH`, both always byte-align and always emit the marker. A stream that uses `Z_BLOCK` is therefore handled by zlib. The one exception is a stream that has already started on IGZIP under a different flush value and then switches to `Z_BLOCK` mid-stream: ISA-L holds unflushed stream state at that point and the stream cannot be moved to zlib without corrupting the output, so `Z_BLOCK` is treated as `Z_SYNC_FLUSH` (byte-aligned, with the extra sync marker). The result is still valid deflate that any decompressor accepts; only an application parsing block boundaries itself can observe the difference.
  - `Z_PARTIAL_FLUSH` is treated as `Z_SYNC_FLUSH`, which zlib permits.
  - `deflateCopy` returns `Z_STREAM_ERROR` for a stream that is mid-stream on IGZIP — one that has called `deflate` but has not yet reached `Z_STREAM_END` — leaving both streams untouched. ISA-L has emitted a header and still holds unflushed stream state, and that state cannot be duplicated: its level buffer holds pointers into its own allocation, so a copy of it would share the source's pending output. Flushing that pending output first is not an alternative either, since those bytes belong to the prefix the two streams share and `deflateCopy` has no way to return bytes to the caller. A stream that has not yet called `deflate`, and a finished one (ISA-L has delivered all its output, so there is nothing left to duplicate and the copy inherits the terminal state), both copy normally — so copying to size the output, or to compress the same input under different settings, still works. Branching a common prefix into several alternative tails requires copying after the prefix has been fed, so it needs a source that is not on IGZIP: such a stream is handled by zlib or by QAT/IAA (which offload `Z_FINISH` only, so a mid-stream call has already pinned it to zlib) and copies without restriction. `inflateCopy` is unrestricted on every path, at any point in a stream.

All backends
- The `strategy` argument of `deflateInit2`/`deflateParams` is not honored by any backend (QAT, IAA, or IGZIP). Compressed output remains valid and round-trips correctly — zlib defines strategy as affecting "the compression ratio but not the correctness of the compressed output" — but the ratio tuning requested by `Z_HUFFMAN_ONLY`, `Z_RLE`, `Z_FIXED`, or `Z_FILTERED` is silently ignored. Applications that depend on a specific strategy for output size or entropy characteristics should disable offload for those streams.
- `Z_NO_COMPRESSION` (level 0) streams are always handled by zlib. Level 0 requests stored, uncompressed deflate blocks, which none of the backends can produce — ISA-L's own level 0 is still LZ77+Huffman compression, and QAT and IAA take no compression level at all — so such streams are routed to zlib rather than being silently compressed. A level change made *after* initialization via `deflateParams` is observed as well, so a stream initialized at level 1-9 and later set to level 0 is routed to zlib from that point on. There are two exceptions:
  - A stream that has already started on IGZIP stays on IGZIP for the rest of that stream, and a level change made while it is running — including a drop to level 0 — is not honored. ISA-L has already emitted a header and compressed data and still holds unflushed stream state at that point, so the stream cannot be moved to zlib without corrupting the output — the same constraint that makes `Z_BLOCK` alias to `Z_SYNC_FLUSH` mid-stream. The output remains valid deflate that round-trips correctly; where the application asked for stored blocks it is compressed instead. `deflateReset` ends the stream, and the next stream on the same `z_stream` does use the new level.
  - `gzsetparams` is not intercepted, so a level set through the `gz*` API is not observed at all.
- `inflate()` with `flush` set to `Z_BLOCK` or `Z_TREES` is not offloadable, and such streams are handled by zlib. zlib defines both as requesting an early return at a deflate block (or, for `Z_TREES`, block-header) boundary, and documents `z_stream.data_type` as reporting the bit position reached. None of the three backends can do either — QAT and IAA decompress whole streams in one submission with no notion of a block boundary, and ISA-L transits its `ISAL_BLOCK_NEW_HDR`/`ISAL_BLOCK_HDR` states inside a single `isal_inflate` call with no way to stop there — so the stream is routed to zlib, which honors both the early return and `data_type`. The routing decision is per stream, not per call: once a stream uses either flush value it stays on zlib, because the bit accounting such a caller performs spans the whole stream. The one exception is a stream already being decompressed by IGZIP that switches to `Z_BLOCK`/`Z_TREES` mid-stream: ISA-L holds unflushed stream state at that point and the stream cannot be moved to zlib without corrupting the output, so those calls behave as `Z_NO_FLUSH` (decompression continues past the boundary). Output is byte-exact either way; only the return timing differs.
- `z_stream.data_type` is not updated on an offloaded `inflate()` call under any other flush value. zlib documents it as being set "every time inflate() returns for all flush options", so an application reading it after an offloaded call sees whatever value it already held. No backend exposes the bit-level accounting needed to compute the field faithfully — QAT and IAA expose no bit position at all — and a partially-correct `data_type` would be indistinguishable from a correct one to the caller, so the field is left unwritten rather than guessed. Decompressed output is unaffected. Applications that track bit positions themselves should either use `Z_BLOCK`/`Z_TREES`, which routes them to zlib as described above, or disable offload for those streams.

CI for HW offload tests is in development (tests are currently run internally).


## Releases

The project is still in development and subject to change. It can be used for testing, but it is not yet ready for production use.

Tagged releases will be provided with details on the maturity of the features. Commits on the main branch that are not tagged as releases are not to be considered stable.


## Build the Shared Library

```
mkdir build
cd build
cmake <options> ..
make
```

CMake supports the following options:
- USE_QAT (ON/OFF): include QAT acceleration
- USE_IAA (ON/OFF): include IAA acceleration
- USE_IGZIP (ON/OFF): include IGZIP acceleration (requires ISA-L)
- QPL_PATH: path to QPL for IAA acceleration (if not in a standard directory)
- QATZIP_PATH: path to QATzip for QAT acceleration (if not in a standard directory)
- ISAL_PATH: path to ISA-L for IGZIP acceleration (if not in a standard directory). May be either an ISA-L build tree or an install prefix.
- DEBUG_LOG (ON/OFF): enable logging
- ENABLE_STATISTICS (ON/OFF): enable statistics
- COVERAGE (ON/OFF): enable test coverage (more details in a later section)
- CMAKE_BUILD_TYPE (Debug/Release...)

For a release build, the following options are recommended: 

```
-DDEBUG_LOG=OFF -DCOVERAGE=OFF -CMAKE_BUILD_TYPE=Release
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
- [ISA-L (Intel Intelligent Storage Acceleration Library)](https://github.com/intel/isa-l)

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

The CMake options are the same as for the shared library build.


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

The CMake options are the same as for the shared library build. Clang is required.
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
- Values: 0,1. Default: 1
- Enable IAA for compression

use_iaa_uncompress
- Values: 0,1.Default: 1
- Enable IAA for decompression

use_zlib_compress
- Values: 0,1. Default: 1
- Enable zlib for compression
- Setting to 1 is recommended, to allow fall back to zlib in case accelerators cannot be used or experience an error.

use_zlib_uncompress
- Values: 0,1. Default: 1
- Enable zlib for decompression
- Setting to 1 is recommended, to allow fall back to zlib in case accelerators cannot be used or experience an error.

igzip_fallback
- Values: 0,1. Default: 1
- If 1, and an IAA or QAT compression/decompression operation fails, the request is retried using IGZIP (if enabled) before falling back to software zlib. Useful on machines where hardware accelerators are intermittently unavailable.

iaa_compress_percentage
- Values: 0-100. Default: 50
- If both IAA and QAT are enabled, percentage of compression calls to offload to IAA.

iaa_prepend_empty_block
- Values: 0,1. Default: 0
- **Deprecated.** This option is retained for backward compatibility and will be removed in a future release. Setting it to 1 has no effect on decompression.
- Background: the original design prepended a 5-byte empty stored-block marker to IAA-compressed output so the decompressor could identify IAA-produced data (which uses a 4kB history window). This approach was abandoned because QPL hardware always consumes all `available_in` bytes regardless of where the stream boundary falls, making marker-based detection unreliable when the caller does not supply the exact compressed size. IAA decompression eligibility is now determined by a 512-byte minimum input length threshold: callers such as Java's `ZipInputStream` feed chunks of ≤512 bytes when the compressed size is unknown, while Lucene stored-field reads always supply the exact size (>512 bytes).

iaa_uncompress_percentage
- Values: 0-100. Default: 50
- If both IAA and QAT are enabled, percentage of decompression calls to offload to IAA.

qat_periodical_polling = 0
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
- Migration note: the numeric meanings changed from earlier versions. Older configurations that used `log_level=2` for error-only output must now use `log_level=1`. Review existing `log_level` settings when upgrading.

log_stats_samples
- Values: 0-INT_MAX. Default 1000
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
- It must be a power of two, so Fibonacci hashing can be used to calculate uniformly distributed shard indexes.

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
- deflateInit, deflateInit2, deflateSetDictionary, deflateParams, deflateCopy, deflate, deflateEnd, deflateReset, deflateResetKeep
- inflateInit, inflateInit2, inflateSetDictionary, inflateCopy, inflate, inflateEnd, inflateReset, inflateReset2, inflateResetKeep

For deflate, offload is supported for Z_FINISH flush option. Support for additional options will be added in later releases.   
For deflateSetDictionary/inflateSetDictionary, zlib-accel simply sets the execution path to zlib, as dictionary compression is currently not supported for accelerators.   
deflateParams is intercepted only to keep the recorded compression level current, so that a level set after initialization is still seen by path selection, and to give up an IGZIP stream that was built for a level the call supersedes; the call itself is always forwarded to zlib.   
inflateReset2 is intercepted because it is the only zlib entry point that changes `windowBits` on a live stream: the recorded window size and format have to be refreshed, or path selection keeps deciding on the format the stream was initialized with, and an IGZIP decompression state built for the old format is discarded rather than reset (ISA-L's reset deliberately preserves its window and format settings). It also clears the terminal state described below, since a stream it restarts is ready to decode again. As with `deflateParams`, the call is forwarded to zlib first and the recorded state is only updated when zlib accepts the new `windowBits`.   
Once a stream has returned `Z_STREAM_END`, `deflate`/`inflate` reproduce zlib's own behavior for every later call on every path: no input is consumed, no output is written, and the return code is what zlib returns — for `deflate`, `Z_STREAM_END` for `Z_FINISH` with no remaining input, `Z_BUF_ERROR` when input remains or there is no output room, and `Z_STREAM_ERROR` for any other flush; for `inflate`, `Z_STREAM_END` for every flush. This matters because an offloaded stream never feeds zlib's own deflate/inflate state, so without it a call after `Z_STREAM_END` would be dispatched from scratch and could append a second stream to a finished one. `deflateReset`, `inflateReset`, `inflateReset2`, `deflateResetKeep`, and `inflateResetKeep` clear the terminal state, and `deflateCopy`/`inflateCopy` carry it to the copy, so a copy of a finished stream refuses input exactly as its source does.   
`deflateResetKeep`/`inflateResetKeep` (declared in zlib.h among the functions zlib does not document) are intercepted because they restart a stream without going through `deflateReset`/`inflateReset`, so a terminal state they left set would wedge the stream at `Z_STREAM_END`. `inflateResetKeep` additionally pins the stream to zlib: keeping the window is the only reason to call it rather than `inflateReset`, so the next stream may reference the previous stream's bytes, which no accelerator can see — the same treatment `inflateSetDictionary` gets. A later `inflateReset` lifts the pin, being the reset that discards the history. `deflateResetKeep` needs no pin: what it keeps only affects how zlib would encode the next stream, and an offloaded stream emits a self-contained one instead. Both are forwarded to zlib first, and the recorded state is only updated when zlib reports success.   
deflateCopy/inflateCopy are intercepted so that the copy gets its own per-stream state: zlib duplicates the stream it owns, but zlib-accel keys its own state on the `z_stream` pointer, so without this the copy would be unknown to the shim and silently run on zlib. The copy inherits the settings and execution path of the source, and for inflate it also gets an independent copy of the IGZIP decompression state, so either stream can be used, reset, or ended without affecting the other. `inflateCopy` is supported on every path; `deflateCopy` has one restriction, described under IGZIP above.

utility functions
- compress, uncompress
- compress, uncompress2

gzip file functions
- gzopen, gzdopen, gzwrite, gzread, gzclose, gzeof


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
