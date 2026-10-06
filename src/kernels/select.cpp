#include "kernels/select.h"

#include "kernels/cpu.h"

#include <immintrin.h>

#include <array>
#include <cmath>

namespace cdb::kernels {

namespace {

// ---------------------------------------------------------------------------------- scalar
// reference

template <class T> bool IsLess(T a, T b) {
    if constexpr (std::is_same_v<T, double>) {
        return a < b || (!std::isnan(a) && std::isnan(b));
    } else {
        return a < b;
    }
}

template <class T> bool IsEqual(T a, T b) {
    if constexpr (std::is_same_v<T, double>) {
        return a == b || (std::isnan(a) && std::isnan(b));
    } else {
        return a == b;
    }
}

template <class T> bool Matches(CmpOp op, T x, T c) {
    switch (op) {
    case CmpOp::Eq:
        return IsEqual(x, c);
    case CmpOp::Ne:
        return !IsEqual(x, c);
    case CmpOp::Lt:
        return IsLess(x, c);
    case CmpOp::Le:
        return !IsLess(c, x);
    case CmpOp::Gt:
        return IsLess(c, x);
    case CmpOp::Ge:
        return !IsLess(x, c);
    }
    return false;
}

template <class T>
idx_t SelectScalar(CmpOp op, const T* data, idx_t first, idx_t count, T constant, sel_t* out) {
    idx_t n = 0;
    for (idx_t i = first; i < count; i++) {
        out[n] = static_cast<sel_t>(i);
        n += Matches(op, data[i], constant);
    }
    return n;
}

// ---------------------------------------------------------------------------------- AVX2

// For an 8-bit match mask, the positions of its set bits packed to the front (the rest is padding):
// permuting {i, i+1, ..., i+7} by this gives the indices of the matching rows, ready to store.
struct CompactTable {
    std::array<std::array<uint32_t, 8>, 256> perm{};
    constexpr CompactTable() {
        for (unsigned mask = 0; mask < 256; mask++) {
            unsigned k = 0;
            for (unsigned bit = 0; bit < 8; bit++) {
                if (mask >> bit & 1) {
                    perm[mask][k++] = bit;
                }
            }
        }
    }
};
constexpr CompactTable kCompact;

__attribute__((target("avx2"))) inline idx_t Emit(unsigned mask, idx_t i, idx_t n, sel_t* out) {
    const __m256i base = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(i)),
                                          _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
    const __m256i perm =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kCompact.perm[mask].data()));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + n),
                        _mm256_permutevar8x32_epi32(base, perm));
    return n + static_cast<idx_t>(__builtin_popcount(mask));
}

__attribute__((target("avx2"))) idx_t SelectInt32Avx2(CmpOp op, const int32_t* data, idx_t count,
                                                      int32_t constant, sel_t* out) {
    const __m256i c = _mm256_set1_epi32(constant);
    idx_t n = 0, i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i m;
        switch (op) {
        case CmpOp::Eq:
            m = _mm256_cmpeq_epi32(x, c);
            break;
        case CmpOp::Ne:
            m = _mm256_xor_si256(_mm256_cmpeq_epi32(x, c), _mm256_set1_epi32(-1));
            break;
        case CmpOp::Lt:
            m = _mm256_cmpgt_epi32(c, x);
            break;
        case CmpOp::Le:
            m = _mm256_xor_si256(_mm256_cmpgt_epi32(x, c), _mm256_set1_epi32(-1));
            break;
        case CmpOp::Gt:
            m = _mm256_cmpgt_epi32(x, c);
            break;
        default: // Ge
            m = _mm256_xor_si256(_mm256_cmpgt_epi32(c, x), _mm256_set1_epi32(-1));
            break;
        }
        n = Emit(static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(m))), i, n, out);
    }
    return n + SelectScalar(op, data + 0, i, count, constant, out + n);
}

__attribute__((target("avx2"))) inline __m256i Cmp64(CmpOp op, __m256i x, __m256i c) {
    const __m256i ones = _mm256_set1_epi64x(-1);
    switch (op) {
    case CmpOp::Eq:
        return _mm256_cmpeq_epi64(x, c);
    case CmpOp::Ne:
        return _mm256_xor_si256(_mm256_cmpeq_epi64(x, c), ones);
    case CmpOp::Lt:
        return _mm256_cmpgt_epi64(c, x);
    case CmpOp::Le:
        return _mm256_xor_si256(_mm256_cmpgt_epi64(x, c), ones);
    case CmpOp::Gt:
        return _mm256_cmpgt_epi64(x, c);
    default: // Ge
        return _mm256_xor_si256(_mm256_cmpgt_epi64(c, x), ones);
    }
}

__attribute__((target("avx2"))) idx_t SelectInt64Avx2(CmpOp op, const int64_t* data, idx_t count,
                                                      int64_t constant, sel_t* out) {
    const __m256i c = _mm256_set1_epi64x(constant);
    idx_t n = 0, i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i lo =
            Cmp64(op, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i)), c);
        const __m256i hi =
            Cmp64(op, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i + 4)), c);
        const unsigned mask = static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(lo))) |
                              static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(hi)))
                                  << 4;
        n = Emit(mask, i, n, out);
    }
    return n + SelectScalar(op, data, i, count, constant, out + n);
}

// The predicates are chosen so that NaN behaves as the largest value and -0.0 == 0.0, for a
// constant that is not NaN (a NaN constant never reaches here): `x > c` and `x >= c` must also hold
// for a NaN x (unordered => true), `x < c`, `x <= c` and `x == c` must not, and `x != c` must.
__attribute__((target("avx2"))) inline __m256d CmpDouble(CmpOp op, __m256d x, __m256d c) {
    switch (op) {
    case CmpOp::Eq:
        return _mm256_cmp_pd(x, c, _CMP_EQ_OQ);
    case CmpOp::Ne:
        return _mm256_cmp_pd(x, c, _CMP_NEQ_UQ);
    case CmpOp::Lt:
        return _mm256_cmp_pd(x, c, _CMP_LT_OQ);
    case CmpOp::Le:
        return _mm256_cmp_pd(x, c, _CMP_LE_OQ);
    case CmpOp::Gt:
        return _mm256_cmp_pd(x, c, _CMP_NLE_UQ);
    default: // Ge
        return _mm256_cmp_pd(x, c, _CMP_NLT_UQ);
    }
}

__attribute__((target("avx2"))) idx_t SelectDoubleAvx2(CmpOp op, const double* data, idx_t count,
                                                       double constant, sel_t* out) {
    const __m256d c = _mm256_set1_pd(constant);
    idx_t n = 0, i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256d lo = CmpDouble(op, _mm256_loadu_pd(data + i), c);
        const __m256d hi = CmpDouble(op, _mm256_loadu_pd(data + i + 4), c);
        const unsigned mask = static_cast<unsigned>(_mm256_movemask_pd(lo)) |
                              static_cast<unsigned>(_mm256_movemask_pd(hi)) << 4;
        n = Emit(mask, i, n, out);
    }
    return n + SelectScalar(op, data, i, count, constant, out + n);
}

} // namespace

template <>
idx_t SelectConstant<int32_t>(CmpOp op, const int32_t* data, idx_t count, int32_t constant,
                              sel_t* out) {
    return UseAvx2() ? SelectInt32Avx2(op, data, count, constant, out)
                     : SelectScalar(op, data, 0, count, constant, out);
}

template <>
idx_t SelectConstant<int64_t>(CmpOp op, const int64_t* data, idx_t count, int64_t constant,
                              sel_t* out) {
    return UseAvx2() ? SelectInt64Avx2(op, data, count, constant, out)
                     : SelectScalar(op, data, 0, count, constant, out);
}

template <>
idx_t SelectConstant<double>(CmpOp op, const double* data, idx_t count, double constant,
                             sel_t* out) {
    if (UseAvx2() && !std::isnan(constant)) {
        return SelectDoubleAvx2(op, data, count, constant, out);
    }
    return SelectScalar(op, data, 0, count, constant, out);
}

} // namespace cdb::kernels
