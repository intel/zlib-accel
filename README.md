# zlib-accel: Transparent Compression/Decompression Acceleration for Zlib

zlib-accel is a software shim that intercepts zlib calls and offloads compression/decompression jobs to hardware accelerators (where feasible and beneficial for performance).
The shim allows applications to leverage hardware accelerators transparently without code changes. The only requirement is to preload the shim's shared library (e.g., using LD_PRELOAD).

Two accelerators are supported
- Intel(R) QuickAssist Technology (QAT)
- Intel(R) In-Memory Analytics Accelerator (IAA)


## Scope/Constraints

This shim is not a general-purpose replacement for zlib, and it is able to offload compression/decompression jobs in certain conditions. Therefore, not all applications can take advantage of the transparent offload, depending on how they use zlib.  It is important to test thoroughly with your specific application and configuration. The use cases we have tested so far are listed in a section below.

In general, the shim is able to offload zlib calls that complete compression/decompression of one deflate stream in one call. "Streaming" compression/decompression (where compression/decompression is done incrementally) are not currently supported. If the shim is not able to offload a job to an accelerator, it will fall back to zlib, ensuring the application still works correctly.

The shim has only been tested on Linux.


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
- QPL_PATH: path to QPL for IAA acceleration (if not in a standard directory)
- QATZIP_PATH: path to QATzip for QAT acceleration (if not in a standard directory)
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

iaa_compress_percentage
- Values: 0-100. Default: 50
- If both IAA and QAT are enabled, percentage of compression calls to offload to IAA.

iaa_prepend_empty_block
- Values: 0,1. Default: 0
- Prepend an empty stored block to the compressed data to "mark" that the data was compressed by IAA.
- IAA has a 4kB history window limit and it is not able to decompress blocks that use a longer history window (up to 32kB per deflate standard).
- During decompression, this marker indicates that the data was compressed by IAA and is therefore guarateed decompressible by IAA.

iaa_uncompress_percentage
- Values: 0-100. Default: 50
- If both IAA and QAT are enabled, percentage of decompression calls to offload to IAA.
- If iaa_prepend_empty_block = 1, this percentage is only applied to data with the empty block marker.

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
- Values: 0,1,2. Default 2
- This option applies only if the shim is built with DEBUG_LOG=ON.
- If 1, error and info log messages are shown. If 2, only error log messages are shown. If 0, no log messages are shown.

log_stats_samples
- Values: 0-INT_MAX. Default 1000
- Append statistics to log every N samples (this option specifies N).

log_file
- Values: path. Default: /tmp/zlib-accel.log
- This option applies only if the shim is built with DEBUG_LOG=ON or ENABLE_STATISTICS=ON.
- If specified, store log messages in the file. If not specified, log messages are printed to stdout.


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
- deflateInit, deflateInit2, deflateSetDictionary, deflate, deflateEnd, deflateReset
- inflateInit, inflateInit2, inflateSetDictionary, inflate, inflateEnd, inflateReset

For deflate, offload is supported for Z_FINISH flush option. Support for additional options will be added in later releases.   
For deflateSetDictionary/inflateSetDictionary, zlib-accel simply sets the execution path to zlib, as dictionary compression is currently not supported for accelerators.

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
