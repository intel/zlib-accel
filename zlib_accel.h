// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once
#pragma GCC visibility push(default)

#include <zlib.h>

// Visible for testing
enum ExecutionPath { UNDEFINED, ZLIB, QAT, IAA, IGZIP };
ExecutionPath GetDeflateExecutionPath(z_streamp strm);
ExecutionPath GetInflateExecutionPath(z_streamp strm);

// True when the shim's entry for strm owns an ISA-L stream, whatever path the
// stream is on. Deliberately independent of the path: *Reset() clears the path
// while keeping the ISA-L stream, and that combination is what the stream-copy
// tests need to observe.
bool DeflateOwnsIgzipState(z_streamp strm);
bool InflateOwnsIgzipState(z_streamp strm);

#pragma GCC visibility pop
