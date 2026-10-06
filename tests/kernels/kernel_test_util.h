#pragma once

#include "kernels/cpu.h"

#include <gtest/gtest.h>

namespace cdb::test {

// Switches the SIMD kernels on or off for the lifetime of the object (a process-wide switch).
class ScopedSimd {
  public:
    explicit ScopedSimd(bool enabled) { kernels::SetSimdEnabled(enabled); }
    ~ScopedSimd() { kernels::SetSimdEnabled(true); }
    ScopedSimd(const ScopedSimd&) = delete;
    ScopedSimd& operator=(const ScopedSimd&) = delete;
};

// Runs `body(simd_enabled)` once with the scalar kernels and once with AVX2; skipped (with a
// message, not a pass) on a CPU without AVX2, because then there is nothing to compare.
#define CDB_REQUIRE_AVX2()                                                                         \
    do {                                                                                           \
        if (!::cdb::kernels::CpuHasAvx2()) {                                                       \
            GTEST_SKIP() << "this CPU has no AVX2: the scalar fallback is tested elsewhere";       \
        }                                                                                          \
    } while (0)

} // namespace cdb::test
