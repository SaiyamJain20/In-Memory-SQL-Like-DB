#include "kernels/aggregate.h"

#include "kernels/cpu.h"

#include <immintrin.h>

#include <algorithm>

namespace cdb::kernels {

namespace {

int64_t SumInt32Scalar(const int32_t* data, idx_t n) {
    int64_t sum = 0;
    for (idx_t i = 0; i < n; i++) {
        sum += data[i];
    }
    return sum;
}

bool AddSumInt64Scalar(const int64_t* data, idx_t n, int64_t* sum) {
    int64_t acc = *sum;
    for (idx_t i = 0; i < n; i++) {
        if (__builtin_add_overflow(acc, data[i], &acc)) {
            return false;
        }
    }
    *sum = acc;
    return true;
}

CompensatedSum SumDoubleScalar(const double* data, idx_t n) {
    CompensatedSum sum;
    for (idx_t i = 0; i < n; i++) {
        sum.Add(data[i]);
    }
    return sum;
}

template <class T> void MinMaxScalar(const T* data, idx_t n, T* min, T* max) {
    T lo = data[0], hi = data[0];
    for (idx_t i = 1; i < n; i++) {
        lo = std::min(lo, data[i]);
        hi = std::max(hi, data[i]);
    }
    *min = lo;
    *max = hi;
}

__attribute__((target("avx2"))) int64_t SumInt32Avx2(const int32_t* data, idx_t n) {
    __m256i acc = _mm256_setzero_si256(); // four int64 partial sums
    idx_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        acc = _mm256_add_epi64(acc, _mm256_cvtepi32_epi64(_mm256_castsi256_si128(v)));
        acc = _mm256_add_epi64(acc, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 1)));
    }
    alignas(32) int64_t lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3] + SumInt32Scalar(data + i, n - i);
}

// Overflow of a + b (two's complement) iff a and b have the same sign and the sum's sign differs:
// ((a ^ sum) & (b ^ sum)) < 0. Accumulating that sign bit over every lane detects an overflow in
// any of the four partial sums; the lanes are then combined with checked scalar additions.
__attribute__((target("avx2"))) bool AddSumInt64Avx2(const int64_t* data, idx_t n, int64_t* sum) {
    __m256i acc = _mm256_setzero_si256();
    __m256i overflow = _mm256_setzero_si256();
    idx_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        const __m256i s = _mm256_add_epi64(acc, v);
        overflow = _mm256_or_si256(
            overflow, _mm256_and_si256(_mm256_xor_si256(acc, s), _mm256_xor_si256(v, s)));
        acc = s;
    }
    if (_mm256_movemask_pd(_mm256_castsi256_pd(overflow)) != 0) {
        return false;
    }
    alignas(32) int64_t lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);
    int64_t total = *sum;
    for (const int64_t lane : lanes) {
        if (__builtin_add_overflow(total, lane, &total)) {
            return false;
        }
    }
    if (!AddSumInt64Scalar(data + i, n - i, &total)) {
        return false;
    }
    *sum = total;
    return true;
}

// Two accumulators of four lanes, each a (hi, lo) pair: hi' = hi + x, and the rounding error of
// that addition (TwoSum) goes to lo. The lanes are merged in a fixed order at the end.
__attribute__((target("avx2"))) CompensatedSum SumDoubleAvx2(const double* data, idx_t n) {
    __m256d hi0 = _mm256_setzero_pd(), lo0 = _mm256_setzero_pd();
    __m256d hi1 = _mm256_setzero_pd(), lo1 = _mm256_setzero_pd();
    idx_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256d x0 = _mm256_loadu_pd(data + i);
        const __m256d x1 = _mm256_loadu_pd(data + i + 4);
        const __m256d s0 = _mm256_add_pd(hi0, x0);
        const __m256d s1 = _mm256_add_pd(hi1, x1);
        const __m256d b0 = _mm256_sub_pd(s0, hi0);
        const __m256d b1 = _mm256_sub_pd(s1, hi1);
        lo0 = _mm256_add_pd(
            lo0, _mm256_add_pd(_mm256_sub_pd(hi0, _mm256_sub_pd(s0, b0)), _mm256_sub_pd(x0, b0)));
        lo1 = _mm256_add_pd(
            lo1, _mm256_add_pd(_mm256_sub_pd(hi1, _mm256_sub_pd(s1, b1)), _mm256_sub_pd(x1, b1)));
        hi0 = s0;
        hi1 = s1;
    }
    alignas(32) double his[8], los[8];
    _mm256_store_pd(his, hi0);
    _mm256_store_pd(his + 4, hi1);
    _mm256_store_pd(los, lo0);
    _mm256_store_pd(los + 4, lo1);
    CompensatedSum sum;
    for (int lane = 0; lane < 8; lane++) {
        sum.Add(CompensatedSum{his[lane], los[lane]});
    }
    for (; i < n; i++) {
        sum.Add(data[i]);
    }
    return sum;
}

// The accumulators stay in registers and are reduced with shuffles. (An earlier version stored them
// through aligned arrays to reduce in scalar code; GCC then kept the accumulators in those stack
// slots inside the loop, a store-to-load dependency per iteration that made it 3x slower than
// scalar.)
__attribute__((target("avx2"))) void MinMaxInt32Avx2(const int32_t* data, idx_t n, int32_t* min,
                                                     int32_t* max) {
    if (n < 8) {
        return MinMaxScalar(data, n, min, max);
    }
    __m256i lo = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
    __m256i hi = lo;
    idx_t i = 8;
    for (; i + 8 <= n; i += 8) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        lo = _mm256_min_epi32(lo, v);
        hi = _mm256_max_epi32(hi, v);
    }
    __m128i l = _mm_min_epi32(_mm256_castsi256_si128(lo), _mm256_extracti128_si256(lo, 1));
    __m128i h = _mm_max_epi32(_mm256_castsi256_si128(hi), _mm256_extracti128_si256(hi, 1));
    l = _mm_min_epi32(l, _mm_shuffle_epi32(l, _MM_SHUFFLE(1, 0, 3, 2)));
    l = _mm_min_epi32(l, _mm_shuffle_epi32(l, _MM_SHUFFLE(2, 3, 0, 1)));
    h = _mm_max_epi32(h, _mm_shuffle_epi32(h, _MM_SHUFFLE(1, 0, 3, 2)));
    h = _mm_max_epi32(h, _mm_shuffle_epi32(h, _MM_SHUFFLE(2, 3, 0, 1)));
    int32_t mn = _mm_cvtsi128_si32(l), mx = _mm_cvtsi128_si32(h);
    for (; i < n; i++) {
        mn = std::min(mn, data[i]);
        mx = std::max(mx, data[i]);
    }
    *min = mn;
    *max = mx;
}

// AVX2 has no 64-bit min/max: select with a compare.
__attribute__((target("avx2"))) void MinMaxInt64Avx2(const int64_t* data, idx_t n, int64_t* min,
                                                     int64_t* max) {
    if (n < 4) {
        return MinMaxScalar(data, n, min, max);
    }
    __m256i lo = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
    __m256i hi = lo;
    idx_t i = 4;
    for (; i + 4 <= n; i += 4) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        lo = _mm256_blendv_epi8(lo, v, _mm256_cmpgt_epi64(lo, v));
        hi = _mm256_blendv_epi8(hi, v, _mm256_cmpgt_epi64(v, hi));
    }
    int64_t mn = _mm256_extract_epi64(lo, 0), mx = _mm256_extract_epi64(hi, 0);
    mn = std::min({mn, static_cast<int64_t>(_mm256_extract_epi64(lo, 1)),
                   static_cast<int64_t>(_mm256_extract_epi64(lo, 2)),
                   static_cast<int64_t>(_mm256_extract_epi64(lo, 3))});
    mx = std::max({mx, static_cast<int64_t>(_mm256_extract_epi64(hi, 1)),
                   static_cast<int64_t>(_mm256_extract_epi64(hi, 2)),
                   static_cast<int64_t>(_mm256_extract_epi64(hi, 3))});
    for (; i < n; i++) {
        mn = std::min(mn, data[i]);
        mx = std::max(mx, data[i]);
    }
    *min = mn;
    *max = mx;
}

} // namespace

int64_t SumInt32(const int32_t* data, idx_t n) {
    return UseAvx2() ? SumInt32Avx2(data, n) : SumInt32Scalar(data, n);
}

bool AddSumInt64(const int64_t* data, idx_t n, int64_t* sum) {
    return UseAvx2() ? AddSumInt64Avx2(data, n, sum) : AddSumInt64Scalar(data, n, sum);
}

CompensatedSum SumDoubleCompensated(const double* data, idx_t n) {
    return UseAvx2() ? SumDoubleAvx2(data, n) : SumDoubleScalar(data, n);
}

double SumDouble(const double* data, idx_t n) {
    return SumDoubleCompensated(data, n).Value();
}

void MinMaxInt32(const int32_t* data, idx_t n, int32_t* min, int32_t* max) {
    UseAvx2() ? MinMaxInt32Avx2(data, n, min, max) : MinMaxScalar(data, n, min, max);
}

void MinMaxInt64(const int64_t* data, idx_t n, int64_t* min, int64_t* max) {
    UseAvx2() ? MinMaxInt64Avx2(data, n, min, max) : MinMaxScalar(data, n, min, max);
}

} // namespace cdb::kernels
