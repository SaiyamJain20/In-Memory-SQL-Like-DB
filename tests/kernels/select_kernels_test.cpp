#include "kernels/select.h"

#include "kernel_test_util.h"
#include "test_util.h"

#include <cmath>
#include <limits>

namespace cdb {

namespace {

using kernels::CmpOp;
using test::Chance;
using test::RandBelow;
using test::Rng;

const CmpOp kOps[] = {CmpOp::Eq, CmpOp::Ne, CmpOp::Lt, CmpOp::Le, CmpOp::Gt, CmpOp::Ge};
const char* const kOpNames[] = {"=", "<>", "<", "<=", ">", ">="};

// An independent definition of the comparison, straight from the engine's order (not a copy of the
// kernel's code): rank NaN above everything, and treat -0.0 as 0.0.
template <class T> bool Reference(CmpOp op, T x, T c) {
    int cmp;
    if constexpr (std::is_same_v<T, double>) {
        const bool xn = std::isnan(x), cn = std::isnan(c);
        if (xn || cn) {
            cmp = xn == cn ? 0 : (xn ? 1 : -1);
        } else {
            cmp = x < c ? -1 : (x > c ? 1 : 0);
        }
    } else {
        cmp = x < c ? -1 : (x > c ? 1 : 0);
    }
    switch (op) {
    case CmpOp::Eq:
        return cmp == 0;
    case CmpOp::Ne:
        return cmp != 0;
    case CmpOp::Lt:
        return cmp < 0;
    case CmpOp::Le:
        return cmp <= 0;
    case CmpOp::Gt:
        return cmp > 0;
    default:
        return cmp >= 0;
    }
}

template <class T> std::vector<T> Pool();
template <> std::vector<int32_t> Pool<int32_t>() {
    return {0,
            1,
            -1,
            2,
            100,
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max(),
            7,
            -7,
            12345};
}
template <> std::vector<int64_t> Pool<int64_t>() {
    return {0,
            1,
            -1,
            2,
            std::numeric_limits<int64_t>::min(),
            std::numeric_limits<int64_t>::max(),
            1LL << 40,
            -(1LL << 40),
            99};
}
template <> std::vector<double> Pool<double>() {
    return {0.0,
            -0.0,
            1.0,
            -1.0,
            0.5,
            100.0,
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::denorm_min(),
            1e300,
            -1e300,
            24.0,
            23.999999999999996};
}

template <class T> void CheckAllOpsAndLengths(Rng& rng, const char* what) {
    const std::vector<T> pool = Pool<T>();
    for (const T constant : pool) {
        for (const CmpOp op : kOps) {
            for (idx_t n = 0; n <= 70; n++) {
                std::vector<T> data(n);
                for (auto& x : data) {
                    x = pool[RandBelow(rng, pool.size())]; // lots of ties with the constant
                }
                std::vector<sel_t> expected;
                for (idx_t i = 0; i < n; i++) {
                    if (Reference(op, data[i], constant)) {
                        expected.push_back(static_cast<sel_t>(i));
                    }
                }
                for (const bool simd : {false, true}) {
                    if (simd && !kernels::CpuHasAvx2()) {
                        continue;
                    }
                    const test::ScopedSimd mode(simd);
                    std::vector<sel_t> out(n + 16, 0xDEADBEEF);
                    const idx_t got =
                        kernels::SelectConstant<T>(op, data.data(), n, constant, out.data());
                    ASSERT_EQ(got, expected.size())
                        << what << " " << kOpNames[static_cast<int>(op)] << " c=" << constant
                        << " n=" << n << (simd ? " (AVX2)" : " (scalar)");
                    for (idx_t k = 0; k < got; k++) {
                        ASSERT_EQ(out[k], expected[k])
                            << what << " " << kOpNames[static_cast<int>(op)] << " n=" << n;
                    }
                    for (idx_t k = n; k < out.size(); k++) {
                        ASSERT_EQ(out[k], 0xDEADBEEF) << "wrote past `count` entries";
                    }
                }
            }
        }
    }
}

} // namespace

TEST(SelectKernels, Int32MatchesTheDefinitionForEveryOpConstantAndLength) {
    Rng rng(1);
    CheckAllOpsAndLengths<int32_t>(rng, "int32");
}
TEST(SelectKernels, Int64MatchesTheDefinitionForEveryOpConstantAndLength) {
    Rng rng(2);
    CheckAllOpsAndLengths<int64_t>(rng, "int64");
}
TEST(SelectKernels, DoubleMatchesTheTotalOrderIncludingNaNInfinityAndNegativeZero) {
    Rng rng(3);
    CheckAllOpsAndLengths<double>(rng, "double");
}

TEST(SelectKernels, FullVectorsOfRandomDataAgreeBetweenScalarAndAvx2) {
    CDB_REQUIRE_AVX2();
    Rng rng(4);
    for (int round = 0; round < 200; round++) {
        const idx_t n = Chance(rng, 0.5) ? kVectorSize : 1 + RandBelow(rng, kVectorSize);
        std::vector<int64_t> data(n);
        for (auto& x : data) {
            x = static_cast<int64_t>(RandBelow(rng, 50)) - 25;
        }
        const int64_t c = static_cast<int64_t>(RandBelow(rng, 50)) - 25;
        for (const CmpOp op : kOps) {
            std::vector<sel_t> a(n), b(n);
            idx_t na, nb;
            {
                const test::ScopedSimd off(false);
                na = kernels::SelectConstant<int64_t>(op, data.data(), n, c, a.data());
            }
            nb = kernels::SelectConstant<int64_t>(op, data.data(), n, c, b.data());
            ASSERT_EQ(na, nb);
            ASSERT_TRUE(std::equal(a.begin(), a.begin() + static_cast<long>(na), b.begin()));
        }
    }
}

} // namespace cdb
