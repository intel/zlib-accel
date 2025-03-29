// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once
#pragma GCC visibility push(default)

#include <zlib.h>

// Visible for testing
enum ExecutionPath { UNDEFINED, ZLIB, QAT, IAA };
ExecutionPath GetDeflateExecutionPath(z_streamp strm);
ExecutionPath GetInflateExecutionPath(z_streamp strm);

#pragma GCC visibility pop
