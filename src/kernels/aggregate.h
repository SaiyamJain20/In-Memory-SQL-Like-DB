#pragma once

#include "common/compensated_sum.h"
#include "common/types.h"

#include <cstdint>

namespace cdb::kernels {

// Whole-vector aggregates over a contiguous array with no NULLs (ungrouped SUM / MIN / MAX of a
// Flat, all-valid vector). AVX2 when the CPU has it.

// Sum of int32 values as int64; cannot overflow for n < 2^32.
int64_t SumInt32(const int32_t* data, idx_t n);

// Adds the sum of int64 values to *sum and returns true, or returns false (leaving *sum
// unspecified) if the vectorised accumulation overflowed. The AVX2 version keeps four partial sums,
// so a partial sum can overflow when a left-to-right sum would not (and, rarely, the reverse); a
// false return therefore means "undecided", and a caller that must follow sequential semantics
// re-runs the sequential checked sum on false. A true return is always exactly start + sum(data),
// and the kernel never returns true when that total does not fit in int64.
bool AddSumInt64(const int64_t* data, idx_t n, int64_t* sum);

// Sum of doubles, accumulated in compensated form (common/compensated_sum.h): the additions are
// re-associated across SIMD lanes, but no rounding error is lost, so the result does not depend on
// the lanes, the order or the CPU. NaN and infinities propagate as usual.
CompensatedSum SumDoubleCompensated(const double* data, idx_t n);
// The same, rounded to a double.
double SumDouble(const double* data, idx_t n);

// Minimum and maximum of n >= 1 values.
void MinMaxInt32(const int32_t* data, idx_t n, int32_t* min, int32_t* max);
void MinMaxInt64(const int64_t* data, idx_t n, int64_t* min, int64_t* max);

} // namespace cdb::kernels
