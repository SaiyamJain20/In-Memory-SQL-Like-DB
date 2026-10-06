#pragma once

namespace cdb::kernels {

// Runtime dispatch for the hand-vectorised kernels. The build never uses -march=native: every AVX2
// function is compiled with a per-function target attribute and is only called when the CPU reports
// AVX2 (and SIMD has not been switched off), so the same binary runs everywhere with the scalar
// fallback. The scalar versions are also the reference the AVX2 ones are tested against.

// True if the CPU supports AVX2 and SIMD kernels are enabled.
bool UseAvx2() noexcept;

// True if the CPU supports AVX2, whether or not SIMD kernels are enabled (tests skip when false).
bool CpuHasAvx2() noexcept;

// Process-wide switch (default: on, unless the environment variable CDB_NO_SIMD is set). Used by
// tests to compare both implementations and by benchmarks to measure the difference.
void SetSimdEnabled(bool enabled) noexcept;

} // namespace cdb::kernels
