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
  - `gzsetparams` is intercepted, and needs no such exception: the `gz*` write path compresses each buffered chunk as a complete gzip member, so the data buffered under the old level is written out as its own member before the new level is recorded, and a change to level 0 routes the rest of the file to zlib. The level given in the `gzopen`/`gzdopen` mode string is observed the same way, including `"wb0"`, which routes the whole file to zlib.
- A compression level of 1-9 is a ratio hint, and only zlib and IGZIP act on it. QAT compresses at the level given by the `qat_compression_level` config option, whatever level the stream asked for, and IAA takes no compression level at all — so on those two paths every level in 1-9 produces the same output. IGZIP does act on it, but maps the range onto ISA-L's three levels (1-2, then 3-6 and `Z_DEFAULT_COMPRESSION`, then 7-9), so neighbouring zlib levels can coincide there as well. Compressed output remains valid and round-trips correctly; only the ratio differs from the one requested, as with `strategy` above. This holds wherever the level comes from — `deflateInit2`, `deflateParams`, the `gzopen`/`gzdopen` mode string, or `gzsetparams` — because it is the backend that has no level to set, not the entry point that failed to record one. Applications that depend on a specific level for output size should disable offload for those streams. Level 0 is a different case, since it changes the format rather than the ratio: it is routed to zlib and honored, as described in the bullet above.
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
- inflateInit, inflateInit2, inflateSetDictionary, inflateCopy, inflate, inflateEnd, inflateReset, inflateReset2, inflateResetKeep, inflateSync

For deflate, offload is supported for Z_FINISH flush option. Support for additional options will be added in later releases.   
For deflateSetDictionary/inflateSetDictionary, zlib-accel simply sets the execution path to zlib, as dictionary compression is currently not supported for accelerators.   
deflateParams is intercepted only to keep the recorded compression level current, so that a level set after initialization is still seen by path selection, and to give up an IGZIP stream that was built for a level the call supersedes; the call itself is always forwarded to zlib.   
inflateReset2 is intercepted because it is the only zlib entry point that changes `windowBits` on a live stream: the recorded window size and format have to be refreshed, or path selection keeps deciding on the format the stream was initialized with, and an IGZIP decompression state built for the old format is discarded rather than reset (ISA-L's reset deliberately preserves its window and format settings). It also clears the terminal state described below, since a stream it restarts is ready to decode again. As with `deflateParams`, the call is forwarded to zlib first and the recorded state is only updated when zlib accepts the new `windowBits`.   
Once a stream has returned `Z_STREAM_END`, `deflate`/`inflate` answer every later call from that terminal state instead of dispatching it to an engine. What matches zlib is the return code, the `msg` string, and the fact that no input is consumed and no output is written — for `deflate`, `Z_STREAM_END` for `Z_FINISH` with no remaining input, `Z_BUF_ERROR` when input remains or there is no output room, and `Z_STREAM_ERROR` for any other flush or for a null `next_out`/`next_in`; for `inflate`, `Z_STREAM_END` for every flush, and `Z_STREAM_ERROR` for a null `next_out` or for a null `next_in` with input pending. `msg` follows zlib's own rule of writing it only where zlib rejects through its internal `ERR_RETURN` macro, so an out-of-range flush on `deflate` — and every `inflate` rejection, which zlib returns without setting `msg` — leaves the field as the caller left it. `data_type` is *not* written, for the reason given above: no offloaded call can compute it, and this gate is no better placed to guess. These calls are counted separately as `deflate_stream_end_count`/`inflate_stream_end_count` when built with `ENABLE_STATISTICS`, so the per-engine counters still add up to `deflate_count`/`inflate_count`. This matters because an offloaded stream never feeds zlib's own deflate/inflate state, so without it a call after `Z_STREAM_END` would be dispatched from scratch and could append a second stream to a finished one. `deflateReset`, `inflateReset`, `inflateReset2`, `deflateResetKeep`, and `inflateResetKeep` clear the terminal state, and `deflateCopy`/`inflateCopy` carry it to the copy, so a copy of a finished stream refuses input exactly as its source does.   
The gate covers `deflate`/`inflate` only. Two other entry points called on a finished stream still report a different return code than zlib would, in either direction, without moving any bytes and without changing how the stream answers a later `Z_FINISH`. `deflateParams` returns `Z_OK` where zlib returns `Z_STREAM_ERROR`: zlib reaches that error through an internal `deflate(strm, Z_BLOCK)` that it only performs once its own encoder has flushed something, and an offloaded stream never advanced that state. `deflateSetDictionary` returns `Z_STREAM_ERROR` where zlib returns `Z_OK`: zlib accepts a dictionary on a finished stream, having cleared the wrapper flag that its "before compression begins" check tests when it wrote the trailer, whereas the mid-stream rejection above also catches a stream that is merely finished. Applications that inspect these return codes on a finished stream should disable offload for those streams.   
`deflateResetKeep`/`inflateResetKeep` (declared in zlib.h among the functions zlib does not document) are intercepted because they restart a stream without going through `deflateReset`/`inflateReset`, so a terminal state they left set would wedge the stream at `Z_STREAM_END`. `inflateResetKeep` additionally pins the stream to zlib: keeping the window is the only reason to call it rather than `inflateReset`, so the next stream may reference the previous stream's bytes, which no accelerator can see — the same treatment `inflateSetDictionary` gets. A later `inflateReset` lifts the pin, being the reset that discards the history. `deflateResetKeep` needs no pin: what it keeps only affects how zlib would encode the next stream, and an offloaded stream emits a self-contained one instead. Both are forwarded to zlib first, and the recorded state is only updated when zlib reports success.   
The pin decides which engine decodes the next stream; it cannot supply the history. If the previous stream was offloaded, zlib's window never received it, so a next stream that does reference those bytes fails with `Z_DATA_ERROR` where unaccelerated zlib would have decoded it. This is a limitation of offloading rather than of the pin: the bytes exist only in the output the accelerator already returned to the caller, and whether a later stream will reference them is unknowable while the previous one is still being decoded. The pin is what makes that case fail as a zlib data error on the stream that needs the history, instead of an accelerator decoding the lookback against an unrelated window and reporting success. Applications that carry decode history across streams this way should disable offload for those streams. Used as a plain restart — the history-independent case — `inflateResetKeep` decodes correctly on every path, but the pin still applies: the stream stays on zlib until an `inflateReset` lifts it, so an application that restarts per message with `inflateResetKeep` gets no acceleration for the life of that `z_stream`. Any IGZIP decompression state the stream held is released at the pin rather than kept for a path that can no longer be selected, and a later `inflateReset` builds a new one.   
When a decode fails part-way through a stream, `inflate` reports `Z_DATA_ERROR` after handing back the bytes the engine had already decoded in that call, and every later call on the stream reports `Z_DATA_ERROR` too — the same way zlib latches a data error. The failed stream is *not* handed to zlib to finish: zlib's own inflate state never saw the earlier chunks, so the input left in `next_in` begins inside a deflate block it would read as the start of a new stream, which on a raw deflate stream means junk counted as output and, at some input alignments, a `Z_STREAM_END` on a short decode. A failure on the *first* call of a stream is different and still goes to zlib, which can take the stream from its start. Only a stateful engine can fail mid-stream, so this applies to IGZIP, including where QAT or IAA reached it through `igzip_fallback`. The latched calls are counted as `inflate_failed_stream_count` when built with `ENABLE_STATISTICS`, for the same reason `inflate_stream_end_count` exists: no engine executes them, and the per-engine counters still have to add up to `inflate_count`.   
`inflateSync` is intercepted because it is the one call that legitimately resumes a stream somewhere other than where the shim left it: it searches the input for a full-flush point and discards the decode state, so decoding continues there with no history. A stream that has already had input consumed is pinned to zlib at that point, and any latched data error is cleared. zlib performed the search, so zlib's state is the one left at the flush point; an accelerator still holds the bits it read ahead of the failure, and whether it resumes correctly would depend on how far it happened to have read. As with the `inflateResetKeep` pin, the stream stays on zlib for the rest of its life, and any IGZIP decompression state it held is released at the next `inflate`. A stream that has consumed nothing keeps its engine — it is still at its own start.   
deflateCopy/inflateCopy are intercepted so that the copy gets its own per-stream state: zlib duplicates the stream it owns, but zlib-accel keys its own state on the `z_stream` pointer, so without this the copy would be unknown to the shim and silently run on zlib. The copy inherits the settings and execution path of the source, and for inflate it also gets an independent copy of the IGZIP decompression state, so either stream can be used, reset, or ended without affecting the other. `inflateCopy` is supported on every path; `deflateCopy` has one restriction, described under IGZIP above.

utility functions
- compress, uncompress
- compress, uncompress2

gzip file functions
- gzopen, gzdopen, gzclose, gzclose_r, gzclose_w, gzeof
- gzwrite, gzputc, gzputs, gzfwrite, gzprintf, gzvprintf, gzflush, gzsetparams
- gzread, gzgetc, gzgetc_, gzgets, gzfread, gzungetc

zlib's `gz*` API is a streaming one: zlib keeps a single deflate or inflate stream per file, plus its own buffer, across every call the application makes. zlib-accel does not stream it. On the write side it buffers what the application writes and compresses each buffer into a *complete gzip member*, so a large file becomes a sequence of members where zlib produces one. That is valid gzip and decompresses normally with any tool; what it costs is compression ratio, since no match reaches across a member boundary. On the read side a member has to decompress within one internal buffer, or the rest of the file is decompressed by zlib.

Every function that moves bytes is therefore intercepted and served from zlib-accel's own buffer. A call that reached zlib instead would act on zlib's stream for that file, which has never seen any of the file's data and whose position bears no relation to the file offset zlib-accel has reached — on the write side it would interleave a second member with the ones zlib-accel wrote, and on the read side it would return bytes decoded from the middle of a compressed member. `gzputc`, `gzputs`, `gzfwrite` and `gzprintf`/`gzvprintf` therefore write through the same path as `gzwrite`, and `gzgetc`, `gzgetc_`, `gzgets` and `gzfread` read through the same path as `gzread`. `gzprintf`/`gzvprintf` format the string themselves rather than handing the file to zlib to format it, which would leave the rest of the file unaccelerated. Formatting it here also removes zlib's length limit, which is a deliberate divergence: zlib formats into its own buffer, and a result that does not fit is not written at all — the call returns 0, having silently discarded the output. zlib-accel writes the whole string whatever its length. The bound is a property of zlib's buffer rather than of the format request, and it is adjustable through `gzbuffer`, which zlib-accel does not intercept, so an application that relies on the limit as a limit should not use gz offload.

`gzflush` writes the buffered data out as a complete member, which is what makes it visible to a reader, and satisfies every flush value zlib accepts; it is deliberately not forwarded to zlib, whose own flush would write a gzip header for a stream holding none of this file's data. `gzungetc` pushes bytes back onto the file without a limit, which `gzread` returns ahead of anything else — most recently pushed first — and it clears the end-of-file indicator, so `gzeof` reports false again until a later read comes up short, as it does with zlib. zlib bounds its push-back by the room in its own output buffer, and guarantees at least a full buffer's worth immediately after the file is opened; accepting every push satisfies that guarantee without depending on a buffer size zlib-accel does not share. `gzclose_r` and `gzclose_w` reach the same close path as `gzclose`, which writes out whatever is still buffered, and reject a file opened for the other direction exactly as zlib does; a close that reached zlib instead would report success while discarding the buffered tail of the file. The compression level in the `gzopen`/`gzdopen` mode string and in `gzsetparams` is recorded and reaches path selection and the compressor, which is as far as any level gets here: level 0 routes the file to zlib and is honored, and a level of 1-9 is acted on only where the backend has a level to set (see the level notes under All backends above).

A `gzFile` zlib-accel has no entry for — one the application did not open through `gzopen`/`gzdopen`, including the `NULL` a failed open returns — is forwarded to zlib unchanged, as is a file already on the zlib path.

The rest of zlib's `gz*` API is **not** intercepted. Those functions act on zlib's own state for the file, which does not reflect what zlib-accel has read or written:

| Not intercepted | Consequence on a file zlib-accel owns |
|---|---|
| `gztell`, `gzoffset`, `gzseek`, `gzrewind` | The offsets reported come from zlib's own accounting, which has tracked none of the file's data, so they are wrong. `gzseek` and `gzrewind` additionally move the file descriptor zlib-accel is reading from or writing to, so the calls that follow act at the wrong offset. Reading or writing a file from start to finish is unaffected; an application that seeks within a `gzFile` should disable gz offload. |
| `gzbuffer` | Sets the size of zlib's own buffers. zlib-accel uses its own, so the request has no effect while the file is accelerated. It is also what bounds zlib's `gzprintf` output and `gzungetc` push-back, neither of which is bounded here — see above. |
| `gzerror`, `gzclearerr` | Report zlib's error state for the file, which stays clear even when an accelerated call has failed. The return values of the intercepted calls are the reliable signal. One consequence: zlib latches a failure and refuses every later call on the file, whereas an accelerated file has no such error state, so a call that follows a failed one is attempted. |
| `gzdirect` | Answers from zlib's view of the file. On a read-mode file zlib inspects the header by reading the descriptor itself, and those bytes are then missing from what zlib-accel reads. |
| `gzopen64`, `gzseek64`, `gztell64`, `gzoffset64` | Not exported, so they are handled by zlib alone. Note that zlib.h redirects `gzopen` to `gzopen64` for an application built with `-D_FILE_OFFSET_BITS=64`: such a file is handled by zlib end to end — correct, but not accelerated. |


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
