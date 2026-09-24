// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The block below pushes default visibility across the #include, which is
// what gives every ZEXPORT definition in zlib_accel.cpp the visibility
// deflate()/inflate()/gzopen()/etc. need to be interposed at all -- it is not
// a place to add a test accessor. A declaration added there is exported the
// same way the real zlib API is, indistinguishable from it in the built
// .so's symbol table and with no attribute of its own to say otherwise. Use
// this macro after the pop instead, the same convention iaa.h/qat.h/igzip.h
// use for symbols a test binary needs from an otherwise hidden library.
#define VISIBLE_FOR_TESTING __attribute__((visibility("default")))

#pragma GCC visibility push(default)

#include <zlib.h>

// Visible for testing
enum ExecutionPath { UNDEFINED, ZLIB, QAT, IAA, IGZIP };
ExecutionPath GetDeflateExecutionPath(z_streamp strm);
ExecutionPath GetInflateExecutionPath(z_streamp strm);
// The gz* entry points keep their own per-file state, so the path a gzFile
// landed on is not reachable through the two accessors above. Tests need it to
// tell a case that really exercised an accelerator from one that fell back --
// without it a gz test passes either way and proves nothing.
ExecutionPath GetGzipFileExecutionPath(gzFile file);

// True when the shim's entry for strm owns an ISA-L stream, whatever path the
// stream is on. Deliberately independent of the path: *Reset() clears the path
// while keeping the ISA-L stream, and that combination is what the stream-copy
// tests need to observe.
bool DeflateOwnsIgzipState(z_streamp strm);
bool InflateOwnsIgzipState(z_streamp strm);

#pragma GCC visibility pop

// True once IAA has rejected a block of this stream for referencing a match
// beyond its 4 kB history buffer. Tests need it because the record deliberately
// survives inflateReset(), and nothing else about the stream reveals that it is
// being kept. Always false in a build without IAA support.
VISIBLE_FOR_TESTING bool InflateIAAWindowRejected(z_streamp strm);
