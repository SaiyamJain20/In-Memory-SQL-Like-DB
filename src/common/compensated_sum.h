#pragma once

#include <cmath>

namespace cdb {

// A floating-point sum kept as an unevaluated sum `hi + lo` of two doubles: every addition is an
// error-free transformation (Knuth's TwoSum) whose rounding error is collected in `lo` instead of
// being lost (Neumaier's improvement of Kahan summation). The result is the sum rounded once, with
// an error of about n * 2^-106 relative, so two additions of the same numbers in *different orders*
// - by different threads, by different SIMD lanes, in a different grouping - round to the same
// double unless the exact sum lies within 2^-100 of a rounding boundary. That is what makes SUM and
// AVG of a DOUBLE column independent of the thread count (a plain parallel sum differs in the last
// bits from run to run, which breaks `x = (SELECT max(x) ...)` over a recomputed aggregate: TPC-H
// Q15).
//
// NaN and infinities propagate as in plain addition (once `hi` is not finite the result is `hi`).
// An intermediate overflow behaves like plain addition too, and is order-dependent.
struct CompensatedSum {
    double hi = 0;
    double lo = 0;

    void Add(double x) noexcept {
        const double s = hi + x;
        const double bb = s - hi;
        lo += (hi - (s - bb)) + (x - bb);
        hi = s;
    }
    // this += other, the two parts combined in a fixed order
    void Add(const CompensatedSum& other) noexcept {
        const double s = hi + other.hi;
        const double bb = s - hi;
        lo += (hi - (s - bb)) + (other.hi - bb) + other.lo;
        hi = s;
    }
    double Value() const noexcept { return std::isfinite(hi) ? hi + lo : hi; }
};

} // namespace cdb
