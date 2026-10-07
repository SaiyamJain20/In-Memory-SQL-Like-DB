#include "storage/hyperloglog.h"

#include "execution/hashing.h"
#include "storage/column_stats.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <string>

namespace cdb {

namespace {

using test::Rng;

// The i-th distinct hash.
uint64_t H(uint64_t i) {
    return HashInt64(i + 1);
}

HyperLogLog Sketch(uint64_t first, uint64_t last) { // hashes of [first, last)
    HyperLogLog s;
    for (uint64_t i = first; i < last; i++) {
        s.Add(H(i));
    }
    return s;
}

} // namespace

TEST(HyperLogLog, AnEmptySketchEstimatesZero) {
    const HyperLogLog s;
    EXPECT_TRUE(s.Empty());
    EXPECT_EQ(s.NonZeroRegisters(), 0U);
    EXPECT_EQ(s.Estimate(), 0.0);
}

TEST(HyperLogLog, EstimatesWithinAFewPercentAtEveryMagnitude) {
    // standard error 1.04 / sqrt(4096) = 1.6%; five sigma of room, and the hashes are fixed, so
    // this is a deterministic check of the estimator including the switch between linear counting
    // and the harmonic mean around 2.5 * 4096 distinct values
    for (const uint64_t n : {1ULL, 2ULL, 3ULL, 10ULL, 100ULL, 1000ULL, 4000ULL, 8000ULL, 10240ULL,
                             12000ULL, 20000ULL, 100000ULL, 1000000ULL}) {
        const double estimate = Sketch(0, n).Estimate();
        const double tolerance = std::max(0.51, 0.08 * static_cast<double>(n));
        EXPECT_NEAR(estimate, static_cast<double>(n), tolerance) << "n = " << n;
    }
    // small counts are essentially exact (linear counting)
    EXPECT_NEAR(Sketch(0, 1).Estimate(), 1.0, 0.01);
    EXPECT_NEAR(Sketch(0, 50).Estimate(), 50.0, 0.5);
}

// Around 2.5 m distinct values the raw harmonic-mean estimator is biased high (+2.6% at 10,000 with
// 4096 registers); linear counting is not. The estimate must stay near the truth across the switch.
TEST(HyperLogLog, TheEstimateIsAccurateAcrossTheSwitchBetweenLinearCountingAndTheHarmonicMean) {
    Rng rng(5);
    for (const uint64_t n : {6000ULL, 8000ULL, 10000ULL, 11000ULL, 12000ULL, 14000ULL, 20000ULL}) {
        double sum_error = 0;
        constexpr int kTrials = 60;
        for (int trial = 0; trial < kTrials; trial++) {
            HyperLogLog s;
            for (uint64_t i = 0; i < n; i++) {
                s.Add(rng());
            }
            sum_error += (s.Estimate() - static_cast<double>(n)) / static_cast<double>(n);
        }
        // the mean error of 60 sketches: linear counting is unbiased, the harmonic mean keeps a
        // bias of about +1% just above the switch (the raw estimator alone: +2.6% at 10,000,
        // +5.9% at 8,000, which this bound rejects)
        EXPECT_LT(std::fabs(sum_error / kTrials), 0.015) << "n = " << n;
    }
}

TEST(HyperLogLog, RepeatedValuesCountOnce) {
    HyperLogLog s;
    for (int rep = 0; rep < 1000; rep++) {
        for (uint64_t i = 0; i < 20; i++) {
            s.Add(H(i));
        }
    }
    EXPECT_NEAR(s.Estimate(), 20.0, 0.5);
    EXPECT_TRUE(s == Sketch(0, 20)) << "the sketch does not depend on repetitions";
}

TEST(HyperLogLog, TheEstimateNeverDecreasesWhenValuesAreAdded) {
    HyperLogLog s;
    double previous = 0;
    for (uint64_t i = 0; i < 30000; i++) {
        s.Add(H(i));
        if (i % 97 == 0) {
            const double e = s.Estimate();
            ASSERT_GE(e, previous) << "after " << i + 1 << " values";
            previous = e;
        }
    }
}

TEST(HyperLogLog, MergeIsTheSketchOfTheUnion) {
    Rng rng(41);
    for (int round = 0; round < 40; round++) {
        // three overlapping random sets
        std::set<uint64_t> a, b, c;
        for (int i = 0; i < 3000; i++) {
            a.insert(test::RandBelow(rng, 6000));
            b.insert(test::RandBelow(rng, 6000) + 2000);
            c.insert(test::RandBelow(rng, 200));
        }
        const auto make = [](const std::set<uint64_t>& s) {
            HyperLogLog h;
            for (const uint64_t v : s) {
                h.Add(H(v));
            }
            return h;
        };
        HyperLogLog all;
        for (const auto* s : {&a, &b, &c}) {
            for (const uint64_t v : *s) {
                all.Add(H(v));
            }
        }
        HyperLogLog ab = make(a);
        ab.Merge(make(b));
        HyperLogLog abc = ab;
        abc.Merge(make(c));
        EXPECT_TRUE(abc == all) << "merging the parts equals sketching the union (round " << round
                                << ")";
        // commutative, associative, idempotent
        HyperLogLog ba = make(b);
        ba.Merge(make(a));
        EXPECT_TRUE(ab == ba);
        HyperLogLog bc = make(b);
        bc.Merge(make(c));
        HyperLogLog a_bc = make(a);
        a_bc.Merge(bc);
        EXPECT_TRUE(a_bc == abc);
        HyperLogLog twice = abc;
        twice.Merge(abc);
        EXPECT_TRUE(twice == abc);
        // the estimate of the union tracks the true union
        std::set<uint64_t> u = a;
        u.insert(b.begin(), b.end());
        u.insert(c.begin(), c.end());
        EXPECT_NEAR(abc.Estimate(), static_cast<double>(u.size()),
                    0.08 * static_cast<double>(u.size()))
            << "round " << round;
    }
}

TEST(HyperLogLog, RegistersRoundTripAndAreValidated) {
    const HyperLogLog s = Sketch(0, 5000);
    HyperLogLog back;
    ASSERT_TRUE(HyperLogLog::FromRegisters(s.registers(), back));
    EXPECT_TRUE(back == s);

    HyperLogLog scratch;
    EXPECT_FALSE(HyperLogLog::FromRegisters({}, scratch));
    EXPECT_FALSE(
        HyperLogLog::FromRegisters(std::vector<uint8_t>(HyperLogLog::kRegisters - 1, 0), scratch));
    EXPECT_FALSE(
        HyperLogLog::FromRegisters(std::vector<uint8_t>(HyperLogLog::kRegisters + 1, 0), scratch));
    std::vector<uint8_t> bad(HyperLogLog::kRegisters, 0);
    bad[100] = HyperLogLog::kMaxRegister + 1;
    EXPECT_FALSE(HyperLogLog::FromRegisters(bad, scratch));
    bad[100] = HyperLogLog::kMaxRegister;
    EXPECT_TRUE(HyperLogLog::FromRegisters(bad, scratch));
    EXPECT_TRUE(scratch.registers()[100] == HyperLogLog::kMaxRegister);
}

TEST(HyperLogLog, ExtremeHashesStayInRange) {
    // all zero bits (the longest run of leading zeros) and all one bits
    HyperLogLog s;
    s.Add(0);
    s.Add(~uint64_t{0});
    for (const uint8_t r : s.registers()) {
        ASSERT_LE(r, HyperLogLog::kMaxRegister);
    }
    EXPECT_EQ(s.registers()[0], HyperLogLog::kMaxRegister);
    EXPECT_NEAR(s.Estimate(), 2.0, 0.01);
}

// ---------------------------------------------------------------------------------- per type

namespace {

template <class T>
double Distinct(LogicalType type, const std::vector<T>& values,
                const std::vector<bool>& valid = {}) {
    ValidityMask validity(AlignUp(static_cast<idx_t>(values.size()), kVectorSize));
    for (size_t i = 0; i < valid.size(); i++) {
        if (!valid[i]) {
            validity.SetInvalid(i);
        }
    }
    // a contiguous array of T (std::vector<bool> has none)
    const std::unique_ptr<T[]> array(new T[values.size()]);
    std::copy(values.begin(), values.end(), array.get());
    HyperLogLog s;
    AddToDistinctSketch(s, type, reinterpret_cast<const uint8_t*>(array.get()), validity,
                        values.size());
    return s.Estimate();
}

} // namespace

TEST(DistinctSketch, EqualValuesCountOnceInEveryType) {
    EXPECT_NEAR(Distinct<int32_t>(LogicalType::Integer(), {1, 2, 2, 3, 3, 3, 1}), 3.0, 0.1);
    EXPECT_NEAR(Distinct<int32_t>(LogicalType::Date(), {10, 10, 11}), 2.0, 0.1);
    EXPECT_NEAR(Distinct<int64_t>(LogicalType::BigInt(), {1LL << 40, 1LL << 40, -1, 0}), 3.0, 0.1);
    EXPECT_NEAR(Distinct<bool>(LogicalType::Boolean(), {true, false, true, true}), 2.0, 0.1);
    EXPECT_NEAR(Distinct<bool>(LogicalType::Boolean(), {true, true}), 1.0, 0.1);
    // -0.0 and 0.0 are one value, every NaN is one value, infinities are their own
    const double nan1 = std::numeric_limits<double>::quiet_NaN();
    const double nan2 = -std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_NEAR(Distinct<double>(LogicalType::Double(), {0.0, -0.0, 0.0}), 1.0, 0.1);
    EXPECT_NEAR(Distinct<double>(LogicalType::Double(), {nan1, nan2, nan1}), 1.0, 0.1);
    EXPECT_NEAR(Distinct<double>(LogicalType::Double(), {inf, -inf, 1.5, 1.5, nan1}), 4.0, 0.1);
}

TEST(DistinctSketch, NullRowsAreNotValues) {
    EXPECT_NEAR(Distinct<int32_t>(LogicalType::Integer(), {5, 6, 7, 8}, {true, false, true, false}),
                2.0, 0.1);
    EXPECT_EQ(Distinct<int32_t>(LogicalType::Integer(), {5, 6}, {false, false}), 0.0);
}

TEST(DistinctSketch, StringsCountByContentWhetherInlineOrNot) {
    const std::vector<std::string> pool = {"",
                                           "a",
                                           "twelve bytes",
                                           "thirteen byte",
                                           std::string(40, 'x'),
                                           std::string(40, 'x') + "y",
                                           "twelve bytes"};
    StringHeap heap;
    std::vector<string_t> strings;
    for (const std::string& s : pool) {
        strings.push_back(heap.Add(s));
    }
    // 7 strings, one repeated: 6 distinct values (and the 12 / 13 byte boundary is crossed)
    EXPECT_NEAR(Distinct<string_t>(LogicalType::Varchar(), strings), 6.0, 0.1);
}

} // namespace cdb
