#include "kernels/decode.h"

#include "kernels/cpu.h"

#include <immintrin.h>

namespace cdb::kernels {

namespace {

void OffsetsToInt32Scalar(const uint64_t* off, idx_t n, uint64_t base, int32_t* out) {
    for (idx_t i = 0; i < n; i++) {
        out[i] = static_cast<int32_t>(static_cast<int64_t>(base + off[i]));
    }
}

void OffsetsToInt64Scalar(const uint64_t* off, idx_t n, uint64_t base, int64_t* out) {
    for (idx_t i = 0; i < n; i++) {
        out[i] = static_cast<int64_t>(base + off[i]);
    }
}

void OffsetsToScaledDoubleScalar(const uint64_t* off, idx_t n, int64_t base, double scale,
                                 double* out) {
    const auto b = static_cast<uint64_t>(base);
    for (idx_t i = 0; i < n; i++) {
        out[i] = static_cast<double>(static_cast<int64_t>(b + off[i])) / scale;
    }
}

// (double)off for off < 2^52 without a 64-bit integer conversion instruction (AVX2 has none): OR
// the offset into the mantissa of 2^52 (0x4330000000000000) and subtract 2^52. Then n = off + base
// is an exact double sum (both operands are exact integers < 2^53 in magnitude and so is the
// result).
__attribute__((target("avx2"))) void
OffsetsToScaledDoubleAvx2(const uint64_t* off, idx_t n, int64_t base, double scale, double* out) {
    const __m256i magic_bits = _mm256_set1_epi64x(0x4330000000000000LL);
    const __m256d magic = _mm256_set1_pd(4503599627370496.0); // 2^52
    const __m256d vbase = _mm256_set1_pd(static_cast<double>(base));
    const __m256d vscale = _mm256_set1_pd(scale);
    idx_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256i o = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(off + i));
        const __m256d d = _mm256_sub_pd(_mm256_castsi256_pd(_mm256_or_si256(o, magic_bits)), magic);
        _mm256_storeu_pd(out + i, _mm256_div_pd(_mm256_add_pd(d, vbase), vscale));
    }
    OffsetsToScaledDoubleScalar(off + i, n - i, base, scale, out + i);
}

} // namespace

// There is deliberately no AVX2 version of these two: an AVX2 implementation (lane-crossing
// permutes to narrow 64-bit lanes to 32 bits) measured slower than this plain loop, which the
// compiler already vectorises (BENCHMARKS.md, Phase 5).
void OffsetsToInt32(const uint64_t* off, idx_t n, uint64_t base, int32_t* out) {
    OffsetsToInt32Scalar(off, n, base, out);
}

void OffsetsToInt64(const uint64_t* off, idx_t n, uint64_t base, int64_t* out) {
    OffsetsToInt64Scalar(off, n, base, out);
}

void OffsetsToScaledDouble(const uint64_t* off, idx_t n, int64_t base, double scale, double* out) {
    UseAvx2() ? OffsetsToScaledDoubleAvx2(off, n, base, scale, out)
              : OffsetsToScaledDoubleScalar(off, n, base, scale, out);
}

} // namespace cdb::kernels
