#include "kernels/hash.h"

#include "execution/hashing.h"
#include "kernels/cpu.h"

#include <immintrin.h>

namespace cdb::kernels {

namespace {

// HashInt64 / CombineHash, from execution/hashing.h (kept in sync by the equivalence tests).
constexpr uint64_t kMurmurC1 = 0xff51afd7ed558ccdULL;
constexpr uint64_t kMurmurC2 = 0xc4ceb9fe1a85ec53ULL;
constexpr uint64_t kCombineK = 0x9e3779b97f4a7c15ULL;

template <class T> void HashScalar(const T* data, idx_t n, uint64_t* out, bool combine) {
    for (idx_t i = 0; i < n; i++) {
        const uint64_t h = HashInt64(static_cast<uint64_t>(static_cast<int64_t>(data[i])));
        out[i] = combine ? CombineHash(out[i], h) : h;
    }
}

// a * c mod 2^64 from three 32x32->64 multiplies (AVX2 has no 64-bit low multiply).
__attribute__((target("avx2"))) inline __m256i Mul64(__m256i a, uint64_t c) {
    const __m256i c_lo = _mm256_set1_epi64x(static_cast<long long>(c & 0xFFFFFFFFULL));
    const __m256i c_hi = _mm256_set1_epi64x(static_cast<long long>(c >> 32));
    const __m256i lo = _mm256_mul_epu32(a, c_lo);
    const __m256i cross = _mm256_add_epi64(_mm256_mul_epu32(_mm256_srli_epi64(a, 32), c_lo),
                                           _mm256_mul_epu32(a, c_hi));
    return _mm256_add_epi64(lo, _mm256_slli_epi64(cross, 32));
}

__attribute__((target("avx2"))) inline __m256i Fmix(__m256i x) {
    x = _mm256_xor_si256(x, _mm256_srli_epi64(x, 33));
    x = Mul64(x, kMurmurC1);
    x = _mm256_xor_si256(x, _mm256_srli_epi64(x, 33));
    x = Mul64(x, kMurmurC2);
    return _mm256_xor_si256(x, _mm256_srli_epi64(x, 33));
}

template <bool kCombine>
__attribute__((target("avx2"))) inline void Store(uint64_t* out, __m256i h) {
    if constexpr (kCombine) {
        const __m256i seed = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out));
        h = Fmix(_mm256_add_epi64(Mul64(seed, kCombineK), h));
    }
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out), h);
}

template <bool kCombine>
__attribute__((target("avx2"))) void HashInt32Avx2(const int32_t* data, idx_t n, uint64_t* out) {
    idx_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256i x =
            _mm256_cvtepi32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)));
        Store<kCombine>(out + i, Fmix(x));
    }
    HashScalar(data + i, n - i, out + i, kCombine);
}

template <bool kCombine>
__attribute__((target("avx2"))) void HashInt64Avx2(const int64_t* data, idx_t n, uint64_t* out) {
    idx_t i = 0;
    for (; i + 4 <= n; i += 4) {
        Store<kCombine>(out + i,
                        Fmix(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i))));
    }
    HashScalar(data + i, n - i, out + i, kCombine);
}

} // namespace

void HashInt32Column(const int32_t* data, idx_t n, uint64_t* out) {
    UseAvx2() ? HashInt32Avx2<false>(data, n, out) : HashScalar(data, n, out, false);
}
void HashInt64Column(const int64_t* data, idx_t n, uint64_t* out) {
    UseAvx2() ? HashInt64Avx2<false>(data, n, out) : HashScalar(data, n, out, false);
}
void CombineInt32Column(const int32_t* data, idx_t n, uint64_t* out) {
    UseAvx2() ? HashInt32Avx2<true>(data, n, out) : HashScalar(data, n, out, true);
}
void CombineInt64Column(const int64_t* data, idx_t n, uint64_t* out) {
    UseAvx2() ? HashInt64Avx2<true>(data, n, out) : HashScalar(data, n, out, true);
}

} // namespace cdb::kernels
