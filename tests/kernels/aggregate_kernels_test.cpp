#include "kernels/aggregate.h"

#include "kernel_test_util.h"
#include "test_util.h"

#include <cmath>
#include <limits>

namespace cdb {

namespace {
__extension__ typedef __int128 Int128; // exact reference arithmetic for the overflow tests
using test::Chance;
using test::RandBelow;
using test::Rng;

std::vector<idx_t> Lengths() {
    std::vector<idx_t> v;
    for (idx_t n = 1; n <= 70; n++) {
        v.push_back(n);
    }
    for (const idx_t n : {127, 128, 129, 1000, 2047, 2048}) {
        v.push_back(n);
    }
    return v;
}
} // namespace

TEST(AggregateKernels, Int32SumIsExactAndMinMaxAreCorrect) {
    Rng rng(1);
    for (const bool simd : {false, true}) {
        if (simd && !kernels::CpuHasAvx2()) {
            continue;
        }
        const test::ScopedSimd mode(simd);
        for (const idx_t n : Lengths()) {
            for (int variant = 0; variant < 4; variant++) {
                std::vector<int32_t> data(n);
                for (auto& x : data) {
                    switch (variant) {
                    case 0:
                        x = static_cast<int32_t>(rng());
                        break;
                    case 1:
                        x = std::numeric_limits<int32_t>::max();
                        break;
                    case 2:
                        x = std::numeric_limits<int32_t>::min();
                        break;
                    default:
                        x = static_cast<int32_t>(RandBelow(rng, 5)) - 2;
                        break;
                    }
                }
                int64_t sum = 0;
                int32_t lo = data[0], hi = data[0];
                for (const int32_t x : data) {
                    sum += x;
                    lo = std::min(lo, x);
                    hi = std::max(hi, x);
                }
                ASSERT_EQ(kernels::SumInt32(data.data(), n), sum)
                    << "n " << n << (simd ? " AVX2" : " scalar");
                int32_t kmin, kmax;
                kernels::MinMaxInt32(data.data(), n, &kmin, &kmax);
                ASSERT_EQ(kmin, lo);
                ASSERT_EQ(kmax, hi);
            }
        }
    }
}

TEST(AggregateKernels, Int64MinMaxAndCheckedSum) {
    Rng rng(2);
    for (const bool simd : {false, true}) {
        if (simd && !kernels::CpuHasAvx2()) {
            continue;
        }
        const test::ScopedSimd mode(simd);
        for (const idx_t n : Lengths()) {
            for (int variant = 0; variant < 5; variant++) {
                std::vector<int64_t> data(n);
                for (auto& x : data) {
                    switch (variant) {
                    case 0:
                        x = static_cast<int64_t>(rng()) >> 20; // sums stay in range
                        break;
                    case 1:
                        x = static_cast<int64_t>(RandBelow(rng, 1000)) - 500;
                        break;
                    case 2:
                        x = std::numeric_limits<int64_t>::max() /
                            static_cast<int64_t>(n + 1); // just fits
                        break;
                    case 3:
                        x = static_cast<int64_t>(rng()); // wraps almost surely
                        break;
                    default:
                        x = Chance(rng, 0.5) ? std::numeric_limits<int64_t>::min()
                                             : std::numeric_limits<int64_t>::max();
                        break;
                    }
                }
                // reference: sequential checked sum
                int64_t expected = 0;
                bool overflow = false;
                for (const int64_t x : data) {
                    overflow = overflow || __builtin_add_overflow(expected, x, &expected);
                }
                // a sequential sum can overflow in the middle and come back; the kernel only
                // promises "no false negatives": if the TRUE total over the integers does not fit,
                // it must report it
                Int128 exact = 0;
                for (const int64_t x : data) {
                    exact += x;
                }
                const bool fits = exact >= std::numeric_limits<int64_t>::min() &&
                                  exact <= std::numeric_limits<int64_t>::max();
                int64_t start = 0;
                const bool ok = kernels::AddSumInt64(data.data(), n, &start);
                if (!fits) {
                    ASSERT_FALSE(ok) << "variant " << variant << " n " << n
                                     << (simd ? " AVX2" : " scalar") << ": overflow missed";
                }
                if (ok) {
                    ASSERT_EQ(static_cast<Int128>(start), exact)
                        << "variant " << variant << " n " << n;
                }
                if (!simd && !overflow) { // the scalar version IS the sequential checked sum
                    ASSERT_TRUE(ok) << "variant " << variant << " n " << n;
                    ASSERT_EQ(start, expected);
                }
                int64_t kmin, kmax;
                kernels::MinMaxInt64(data.data(), n, &kmin, &kmax);
                ASSERT_EQ(kmin, *std::min_element(data.begin(), data.end()));
                ASSERT_EQ(kmax, *std::max_element(data.begin(), data.end()));
            }
        }
    }
}

TEST(AggregateKernels, VectorOverflowIsConservativeAndTheSequentialSumDecides) {
    // Left to right this never overflows (M, 0, 0, 0, M, 0, 0, 0 ...), but the four partial sums of
    // the vector version see lane 0 = M + M. The kernel may say "undecided" (false); the caller
    // then runs the sequential checked sum, which is the SQL-visible behaviour.
    constexpr int64_t M = std::numeric_limits<int64_t>::max();
    const int64_t data[8] = {M, -M, 0, 0, M, -M, 0, 0};
    int64_t sequential = 0;
    bool seq_overflow = false;
    for (const int64_t x : data) {
        seq_overflow = seq_overflow || __builtin_add_overflow(sequential, x, &sequential);
    }
    ASSERT_FALSE(seq_overflow);
    ASSERT_EQ(sequential, 0);
    {
        const test::ScopedSimd off(false);
        int64_t sum = 0;
        EXPECT_TRUE(kernels::AddSumInt64(data, 8, &sum))
            << "the scalar kernel IS the sequential sum";
        EXPECT_EQ(sum, 0);
    }
    if (kernels::CpuHasAvx2()) {
        int64_t sum = 0;
        if (kernels::AddSumInt64(data, 8, &sum)) { // deciding is allowed, but then it must be right
            EXPECT_EQ(sum, 0);
        }
    }
    // and a total that really does not fit is never reported as a sum
    const int64_t too_big[8] = {M, 1, 0, 0, 0, 0, 0, 0};
    int64_t sum = 0;
    EXPECT_FALSE(kernels::AddSumInt64(too_big, 8, &sum));
}

TEST(AggregateKernels, SumStartsFromTheGivenValue) {
    const int64_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t sum = 100;
    ASSERT_TRUE(kernels::AddSumInt64(data, 10, &sum));
    EXPECT_EQ(sum, 155);
    int64_t near_max = std::numeric_limits<int64_t>::max() - 54;
    EXPECT_FALSE(kernels::AddSumInt64(data, 10, &near_max)) << "55 more does not fit";
    int64_t exactly = std::numeric_limits<int64_t>::max() - 55;
    EXPECT_TRUE(kernels::AddSumInt64(data, 10, &exactly));
    EXPECT_EQ(exactly, std::numeric_limits<int64_t>::max());
}

TEST(AggregateKernels, DoubleSumIsCloseToSequentialAndPropagatesNonFinites) {
    Rng rng(3);
    for (const bool simd : {false, true}) {
        if (simd && !kernels::CpuHasAvx2()) {
            continue;
        }
        const test::ScopedSimd mode(simd);
        for (const idx_t n : Lengths()) {
            std::vector<double> data(n);
            double sequential = 0, magnitude = 0;
            for (auto& x : data) {
                x = static_cast<double>(static_cast<int64_t>(RandBelow(rng, 2000000)) - 1000000) /
                    100.0;
                sequential += x;
                magnitude += std::fabs(x);
            }
            const double got = kernels::SumDouble(data.data(), n);
            ASSERT_LE(std::fabs(got - sequential), 1e-12 * std::max(1.0, magnitude)) << "n " << n;
            // exactly representable values (multiples of 0.25) sum identically in any order
            std::vector<double> exact(n);
            double want = 0;
            for (auto& x : exact) {
                x = static_cast<double>(RandBelow(rng, 100)) * 0.25;
                want += x;
            }
            ASSERT_EQ(kernels::SumDouble(exact.data(), n), want);
            // non-finite values propagate
            std::vector<double> bad = exact;
            bad[RandBelow(rng, n)] = std::numeric_limits<double>::infinity();
            ASSERT_TRUE(std::isinf(kernels::SumDouble(bad.data(), n)));
            bad[RandBelow(rng, n)] = std::numeric_limits<double>::quiet_NaN();
            ASSERT_TRUE(std::isnan(kernels::SumDouble(bad.data(), n)));
        }
    }
}

} // namespace cdb
