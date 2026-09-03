// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// AddressSanitizer suppressions for the test binary (-DASAN=ON), compiled in
// rather than passed through ASAN_OPTIONS so that a bare ./zlib_accel_test is
// covered as well as the run target.
//
// libstdc++ marks all of namespace std with default visibility, so
// -fvisibility=hidden does not hide the std globals that the test binary and
// libzlib-accel.so each instantiate. Where a linker binds the two copies to one
// address, ASAN sees one global registered twice and reports an ODR violation
// before the first test runs. The definitions are identical: this is structural
// to linking two objects that both use std::shared_ptr and
// std::piecewise_construct, not a defect to chase.
//
// One entry per duplicated global, so ODR detection stays on for everything
// else. ASAN matches these against the bare global name, which is as narrow as
// the runtime allows -- there is no way to scope an entry to a namespace or a
// module. A toolchain that duplicates a further std global aborts on it by
// name, which is the intended failure: the entry is then a decision, not a
// blanket flag.
extern "C" __attribute__((visibility("default"))) const char*
__asan_default_suppressions() {
  return "odr_violation:__tag\n"
         "odr_violation:piecewise_construct\n";
}
