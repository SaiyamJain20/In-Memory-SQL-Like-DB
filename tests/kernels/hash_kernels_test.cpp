#include "kernels/hash.h"

#include "execution/hashing.h"
#include "kernel_test_util.h"
#include "test_util.h"

#include <limits>

namespace cdb {

namespace {
using test::RandBelow;
using test::Rng;

std::vector<idx_t> Lengths() {
    std::vector<idx_t> v;
    for (idx_t n = 0; n <= 70; n++) {
        v.push_back(n);
    }
    for (const idx_t n : {127, 128, 129, 2047, 2048}) {
        v.push_back(n);
    }
    return v;
}
} // namespace

TEST(HashKernels, MatchTheScalarHashForEveryLengthAndValueIncludingSigns) {
    Rng rng(1);
    const int64_t specials[] = {0,
                                1,
                                -1,
                                std::numeric_limits<int64_t>::min(),
                                std::numeric_limits<int64_t>::max(),
                                1LL << 32,
                                -(1LL << 32),
                                0xFFFFFFFFLL};
    for (const idx_t n : Lengths()) {
        std::vector<int32_t> d32(n);
        std::vector<int64_t> d64(n);
        for (idx_t i = 0; i < n; i++) {
            d64[i] = RandBelow(rng, 4) == 0 ? specials[RandBelow(rng, std::size(specials))]
                                            : static_cast<int64_t>(rng());
            d32[i] = RandBelow(rng, 4) == 0
                         ? static_cast<int32_t>(specials[RandBelow(rng, std::size(specials))])
                         : static_cast<int32_t>(rng());
        }
        std::vector<uint64_t> seeds(n);
        for (auto& s : seeds) {
            s = rng();
        }
        for (const bool simd : {false, true}) {
            if (simd && !kernels::CpuHasAvx2()) {
                continue;
            }
            const test::ScopedSimd mode(simd);
            std::vector<uint64_t> h32(n + 4, 7), h64(n + 4, 7), c32 = seeds, c64 = seeds;
            c32.resize(n + 4, 7);
            c64.resize(n + 4, 7);
            kernels::HashInt32Column(d32.data(), n, h32.data());
            kernels::HashInt64Column(d64.data(), n, h64.data());
            kernels::CombineInt32Column(d32.data(), n, c32.data());
            kernels::CombineInt64Column(d64.data(), n, c64.data());
            for (idx_t i = 0; i < n; i++) {
                ASSERT_EQ(h32[i], HashInt64(static_cast<uint64_t>(static_cast<int64_t>(d32[i]))))
                    << "n " << n << " i " << i << (simd ? " AVX2" : " scalar");
                ASSERT_EQ(h64[i], HashInt64(static_cast<uint64_t>(d64[i])));
                ASSERT_EQ(c32[i], CombineHash(seeds[i], HashInt64(static_cast<uint64_t>(
                                                            static_cast<int64_t>(d32[i])))));
                ASSERT_EQ(c64[i], CombineHash(seeds[i], HashInt64(static_cast<uint64_t>(d64[i]))));
            }
            for (idx_t i = n; i < n + 4; i++) {
                ASSERT_EQ(h32[i], 7U) << "wrote past n";
                ASSERT_EQ(h64[i], 7U);
                ASSERT_EQ(c32[i], 7U);
                ASSERT_EQ(c64[i], 7U);
            }
        }
    }
}

TEST(HashKernels, VectorsOfEveryFormatStillHashEqualValuesEqually) {
    // HashVector now takes the kernel path for Flat all-valid integer columns: a value must hash
    // the same there as through the generic path (a dictionary or constant view of the same
    // values).
    Rng rng(2);
    Vector flat(LogicalType::BigInt(), kVectorSize);
    std::vector<int64_t> values(kVectorSize);
    for (idx_t i = 0; i < kVectorSize; i++) {
        values[i] = static_cast<int64_t>(RandBelow(rng, 100)) - 50;
        flat.SetValue(i, Value::BigInt(values[i]));
    }
    SelectionVector sel(kVectorSize);
    for (idx_t i = 0; i < kVectorSize; i++) {
        sel.Set(i, static_cast<sel_t>(kVectorSize - 1 - i));
    }
    Vector dict(LogicalType::BigInt(), kVectorSize);
    dict.Reference(flat);
    dict.Slice(sel, kVectorSize);
    std::vector<uint64_t> a(kVectorSize), b(kVectorSize);
    HashVector(flat, kVectorSize, a.data(), false);
    HashVector(dict, kVectorSize, b.data(), false);
    for (idx_t i = 0; i < kVectorSize; i++) {
        ASSERT_EQ(a[kVectorSize - 1 - i], b[i]);
    }
}

} // namespace cdb
