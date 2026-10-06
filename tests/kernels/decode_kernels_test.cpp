#include "kernels/decode.h"

#include "kernel_test_util.h"
#include "test_util.h"

#include <cstring>

namespace cdb {

namespace {
using test::RandBelow;
using test::Rng;

// Lengths around every vector-width and tail boundary.
std::vector<idx_t> Lengths() {
    std::vector<idx_t> v;
    for (idx_t n = 0; n <= 70; n++) {
        v.push_back(n);
    }
    for (const idx_t n : {127, 128, 129, 1000, 2047, 2048}) {
        v.push_back(n);
    }
    return v;
}
} // namespace

TEST(DecodeKernels, Int32AndInt64ConversionsAreExactForEveryBaseAndLength) {
    Rng rng(1);
    const uint64_t bases[] = {0,
                              1,
                              12345,
                              ~uint64_t{0} /* -1 */,
                              uint64_t{1} << 31,
                              0xFFFFFFFF80000000ULL,
                              uint64_t{1} << 62,
                              0x7FFFFFFFFFFFFFFFULL};
    for (const uint64_t base : bases) {
        for (const idx_t n : Lengths()) {
            std::vector<uint64_t> off(n);
            for (auto& o : off) {
                o = rng() >> RandBelow(rng, 64); // every magnitude, including full 64 bits
            }
            std::vector<int32_t> s32(n + 8, 0x5A5A5A5A), v32(n + 8, 0x5A5A5A5A);
            std::vector<int64_t> s64(n + 4, 0x5A5A), v64(n + 4, 0x5A5A);
            {
                test::ScopedSimd off_simd(false);
                kernels::OffsetsToInt32(off.data(), n, base, s32.data());
                kernels::OffsetsToInt64(off.data(), n, base, s64.data());
            }
            kernels::OffsetsToInt32(off.data(), n, base, v32.data());
            kernels::OffsetsToInt64(off.data(), n, base, v64.data());
            ASSERT_EQ(s32, v32) << "base " << base << " n " << n
                                << " (including that nothing is written past n)";
            ASSERT_EQ(s64, v64) << "base " << base << " n " << n;
            for (idx_t i = 0; i < n; i++) { // and the scalar result is the definition
                ASSERT_EQ(s32[i], static_cast<int32_t>(static_cast<int64_t>(base + off[i])));
                ASSERT_EQ(s64[i], static_cast<int64_t>(base + off[i]));
            }
        }
    }
}

TEST(DecodeKernels, ScaledDoubleConversionIsBitExactForEveryValidInput) {
    CDB_REQUIRE_AVX2();
    Rng rng(2);
    const double scales[] = {1, 10, 100, 1e3, 1e4, 1e5, 1e6};
    const int64_t bases[] = {0,
                             1,
                             -1,
                             123456789,
                             -123456789,
                             -(int64_t{1} << 52),
                             (int64_t{1} << 52) - 1,
                             -(int64_t{1} << 53) + 1,
                             4503599627370495LL};
    for (const double scale : scales) {
        for (const int64_t base : bases) {
            for (const idx_t n : Lengths()) {
                // offsets < 2^52 and base + offset within +-2^53, as the encoder guarantees
                const int64_t room = (int64_t{1} << 53) - 1 - std::max<int64_t>(base, 0);
                const uint64_t limit = std::min<uint64_t>(
                    uint64_t{1} << 52, static_cast<uint64_t>(std::max<int64_t>(room, 1)));
                std::vector<uint64_t> off(n);
                for (auto& o : off) {
                    o = (rng() >> RandBelow(rng, 64)) % limit;
                }
                std::vector<double> s(n + 4, -77.0), v(n + 4, -77.0);
                {
                    test::ScopedSimd off_simd(false);
                    kernels::OffsetsToScaledDouble(off.data(), n, base, scale, s.data());
                }
                kernels::OffsetsToScaledDouble(off.data(), n, base, scale, v.data());
                for (idx_t i = 0; i < n + 4; i++) {
                    ASSERT_EQ(std::memcmp(&s[i], &v[i], sizeof(double)), 0)
                        << "scale " << scale << " base " << base << " n " << n << " i " << i << ": "
                        << s[i] << " vs " << v[i];
                }
            }
        }
    }
}

TEST(DecodeKernels, ScalarVersionsAreUsedWhenSimdIsSwitchedOff) {
    const test::ScopedSimd off(false);
    EXPECT_FALSE(kernels::UseAvx2());
    uint64_t off_vals[5] = {1, 2, 3, 4, 5};
    int32_t out[5];
    kernels::OffsetsToInt32(off_vals, 5, 10, out);
    EXPECT_EQ(out[4], 15);
}

} // namespace cdb
