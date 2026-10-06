// Tests for the column encodings: bit packing, and every encoding of a sealed segment. The core
// property is a round trip: scanning an encoded segment gives exactly the rows that went in
// (bit-exact for doubles), for every vector, in any order, for data shaped to hit each encoding's
// edge cases - constants, runs, sorted keys, extreme values, NULLs everywhere or nowhere, partial
// last vectors.

#include "storage/bitpacking.h"
#include "storage/encoding.h"

#include "storage_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>

namespace cdb {

namespace {

using test::BitIdentical;
using test::Chance;
using test::RandBelow;
using test::Rng;

// ------------------------------------------------------------------------------ helpers

std::shared_ptr<ColumnSegment> MakeRawSegment(LogicalType type, const std::vector<Value>& values) {
    const test::ScopedCompression raw_layout(false);
    ColumnBuilder builder(type, std::max<idx_t>(kVectorSize, AlignUp(values.size(), kVectorSize)));
    Vector src(type, kVectorSize);
    for (idx_t at = 0; at < values.size(); at += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, values.size() - at);
        for (idx_t i = 0; i < n; i++) {
            src.SetValue(i, values[at + i]);
        }
        builder.Append(src, 0, n);
    }
    return builder.Seal();
}

std::shared_ptr<ColumnSegment> WithEncoding(const ColumnSegment& raw,
                                            std::shared_ptr<EncodedColumn> e) {
    return std::make_shared<ColumnSegment>(raw.type(), raw.count(), raw.validity(), raw.stats(),
                                           std::move(e));
}

std::vector<Value> ScanAll(const ColumnSegment& seg, Rng* shuffle = nullptr) {
    std::vector<Value> out(seg.count(), Value::Null(seg.type()));
    std::vector<idx_t> offsets;
    for (idx_t at = 0; at < seg.count(); at += kVectorSize) {
        offsets.push_back(at);
    }
    if (shuffle != nullptr) {
        std::shuffle(offsets.begin(), offsets.end(), *shuffle);
    }
    Vector v(seg.type(), kVectorSize);
    for (const idx_t at : offsets) {
        const idx_t n = std::min<idx_t>(kVectorSize, seg.count() - at);
        seg.Scan(at, n, v);
        v.Verify(n);
        for (idx_t i = 0; i < n; i++) {
            out[at + i] = v.GetValue(i);
        }
    }
    return out;
}

// Encodes `values` with `choice`, checks the round trip, and returns the encoding (null if the
// choice did not apply).
std::shared_ptr<EncodedColumn> RoundTrip(LogicalType type, const std::vector<Value>& values,
                                         EncodingChoice choice, const std::string& what) {
    const auto raw = MakeRawSegment(type, values);
    auto encoded = EncodeSegment(*raw, choice);
    if (encoded == nullptr) {
        return nullptr;
    }
    const auto seg = WithEncoding(*raw, encoded);
    Rng order(values.size() + 1);
    for (const bool shuffled : {false, true}) {
        const std::vector<Value> got = ScanAll(*seg, shuffled ? &order : nullptr);
        for (idx_t i = 0; i < values.size(); i++) {
            if (!BitIdentical(got[i], values[i])) {
                ADD_FAILURE() << what << ": row " << i << " got " << got[i].ToString() << " want "
                              << values[i].ToString()
                              << (shuffled ? " (vectors scanned out of order)" : "");
                return nullptr;
            }
        }
    }
    EXPECT_EQ(seg->stats().count, raw->count());
    return encoded;
}

// ------------------------------------------------------------------------------ data shapes

enum class Shape {
    Constant,
    TwoValues,
    Ascending,
    AscendingJumps,
    Descending,
    NarrowAroundHuge,
    Runs,
    Random,
    Extremes
};
const char* const kShapeNames[] = {"constant",   "two values",
                                   "ascending",  "ascending with jumps",
                                   "descending", "narrow around a huge base",
                                   "runs",       "random",
                                   "extremes"};

int64_t ClampTo(LogicalType type, int64_t v) {
    switch (type.id()) {
    case TypeId::Boolean:
        return v & 1;
    case TypeId::Integer:
    case TypeId::Date:
        return std::clamp<int64_t>(v, std::numeric_limits<int32_t>::min(),
                                   std::numeric_limits<int32_t>::max());
    default:
        return v;
    }
}

Value MakeInt(LogicalType type, int64_t v) {
    v = ClampTo(type, v);
    switch (type.id()) {
    case TypeId::Boolean:
        return Value::Boolean(v != 0);
    case TypeId::Integer:
        return Value::Integer(static_cast<int32_t>(v));
    case TypeId::Date:
        return Value::Date(date_t{static_cast<int32_t>(v)});
    default:
        return Value::BigInt(v);
    }
}

std::vector<Value> IntColumn(Rng& rng, LogicalType type, Shape shape, idx_t n, double null_prob) {
    std::vector<int64_t> x(n);
    const bool small = type.id() == TypeId::Boolean;
    switch (shape) {
    case Shape::Constant:
        std::fill(x.begin(), x.end(), small ? 1 : 123456);
        break;
    case Shape::TwoValues:
        for (auto& v : x) {
            v = RandBelow(rng, 2) ? 7 : -3;
        }
        break;
    case Shape::Ascending: {
        int64_t cur = -1000;
        for (auto& v : x) {
            cur += RandBelow(rng, 4);
            v = cur;
        }
        break;
    }
    case Shape::AscendingJumps: {
        int64_t cur = 0;
        for (auto& v : x) {
            cur += Chance(rng, 0.01) ? static_cast<int64_t>(RandBelow(rng, 1000000))
                                     : static_cast<int64_t>(RandBelow(rng, 3));
            v = cur;
        }
        break;
    }
    case Shape::Descending: {
        int64_t cur = 1000000;
        for (auto& v : x) {
            cur -= RandBelow(rng, 5);
            v = cur;
        }
        break;
    }
    case Shape::NarrowAroundHuge:
        for (auto& v : x) {
            v = type.id() == TypeId::BigInt ? std::numeric_limits<int64_t>::max() -
                                                  static_cast<int64_t>(RandBelow(rng, 100))
                                            : std::numeric_limits<int32_t>::max() -
                                                  static_cast<int64_t>(RandBelow(rng, 100));
        }
        break;
    case Shape::Runs: {
        int64_t cur = 0;
        idx_t left = 0;
        for (auto& v : x) {
            if (left == 0) {
                cur = static_cast<int64_t>(RandBelow(rng, 50)) * 7919;
                left = 1 + RandBelow(rng, 300);
            }
            v = cur;
            left--;
        }
        break;
    }
    case Shape::Random:
        for (auto& v : x) {
            v = static_cast<int64_t>(rng());
        }
        break;
    case Shape::Extremes: {
        const int64_t pool[] = {
            std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), 0, -1, 1,
            std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()};
        for (auto& v : x) {
            v = pool[RandBelow(rng, std::size(pool))];
        }
        break;
    }
    }
    std::vector<Value> out;
    for (idx_t i = 0; i < n; i++) {
        out.push_back(Chance(rng, null_prob) ? Value::Null(type) : MakeInt(type, x[i]));
    }
    return out;
}

const idx_t kCounts[] = {1, 2, 63, 2047, 2048, 2049, 4096, 5000, 10000};

} // namespace

// ------------------------------------------------------------------------------ bit packing

TEST(BitPacking, WidthOfValues) {
    EXPECT_EQ(BitWidth(0), 0);
    EXPECT_EQ(BitWidth(1), 1);
    EXPECT_EQ(BitWidth(2), 2);
    EXPECT_EQ(BitWidth(255), 8);
    EXPECT_EQ(BitWidth(256), 9);
    EXPECT_EQ(BitWidth(~uint64_t{0}), 64);
    EXPECT_EQ(BitWidth(uint64_t{1} << 63), 64);
}

TEST(BitPacking, EveryWidthRoundTripsForEveryLength) {
    Rng rng(1);
    for (int width = 0; width <= 64; width++) {
        for (const idx_t count :
             {0, 1, 2, 3, 7, 8, 9, 31, 32, 33, 63, 64, 65, 127, 129, 2047, 2048}) {
            const uint64_t mask = width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
            std::vector<uint64_t> in(count);
            for (auto& v : in) {
                v = (static_cast<uint64_t>(rng()) & mask);
            }
            if (count > 2) { // make the extremes appear
                in[0] = mask;
                in[count - 1] = mask;
                in[1] = 0;
            }
            const size_t bytes = PackedBytes(count, static_cast<uint8_t>(width));
            // guard bytes after the stream must not be touched by packing
            std::vector<uint8_t> packed(bytes + 16, 0xAB);
            std::fill(packed.begin(), packed.begin() + static_cast<long>(bytes), 0);
            BitPack(in.data(), count, static_cast<uint8_t>(width), packed.data());
            for (size_t k = bytes; k < packed.size(); k++) {
                ASSERT_EQ(packed[k], 0xAB)
                    << "width " << width << " count " << count << " wrote past the stream";
            }
            std::vector<uint64_t> out(count, 0xDEADBEEF);
            BitUnpack(packed.data(), count, static_cast<uint8_t>(width), out.data());
            ASSERT_EQ(in, out) << "width " << width << " count " << count;
        }
    }
}

TEST(BitPacking, HighBitsOfTheInputAreIgnored) {
    const uint64_t in[] = {0xFFFFFFFFFFFFFFFFULL, 0x0, 0xAAAAAAAAAAAAAAAAULL};
    std::vector<uint8_t> packed(PackedBytes(3, 5), 0);
    BitPack(in, 3, 5, packed.data());
    uint64_t out[3];
    BitUnpack(packed.data(), 3, 5, out);
    EXPECT_EQ(out[0], 31U);
    EXPECT_EQ(out[1], 0U);
    EXPECT_EQ(out[2], 0xAAAAAAAAAAAAAAAAULL & 31U);
}

// ------------------------------------------------------------------------------ integers

TEST(IntegerEncodings, EveryEncodingRoundTripsEveryShape) {
    Rng rng(2);
    int encoded_cases = 0;
    for (const LogicalType type : {LogicalType::Boolean(), LogicalType::Integer(),
                                   LogicalType::BigInt(), LogicalType::Date()}) {
        for (const Shape shape : {Shape::Constant, Shape::TwoValues, Shape::Ascending,
                                  Shape::AscendingJumps, Shape::Descending, Shape::NarrowAroundHuge,
                                  Shape::Runs, Shape::Random, Shape::Extremes}) {
            for (const double nulls : {0.0, 0.1, 1.0}) {
                for (const idx_t count : kCounts) {
                    const std::vector<Value> values = IntColumn(rng, type, shape, count, nulls);
                    const std::string what = type.ToString() + " " +
                                             kShapeNames[static_cast<int>(shape)] + " nulls " +
                                             std::to_string(nulls) + " n " + std::to_string(count);
                    for (const EncodingChoice choice :
                         {EncodingChoice::Bitpacked, EncodingChoice::Rle, EncodingChoice::Constant,
                          EncodingChoice::Auto}) {
                        encoded_cases += RoundTrip(type, values, choice, what) != nullptr;
                        if (::testing::Test::HasFailure()) {
                            return;
                        }
                    }
                }
            }
        }
    }
    EXPECT_GT(encoded_cases, 1000) << "the test must actually exercise the encoders";
}

TEST(IntegerEncodings, FirstRowNullAndNullRunsAreCarriedWithoutBreakingRangesOrRuns) {
    std::vector<Value> values;
    for (int i = 0; i < 5000; i++) {
        const bool null = i < 30 || (i % 700) < 25;
        values.push_back(null ? Value::Null(LogicalType::Integer())
                              : Value::Integer(1000 + i / 1000));
    }
    for (const EncodingChoice c :
         {EncodingChoice::Bitpacked, EncodingChoice::Rle, EncodingChoice::Auto}) {
        ASSERT_NE(RoundTrip(LogicalType::Integer(), values, c, "null carry"), nullptr);
    }
}

TEST(IntegerEncodings, ChooserPicksTheRightEncodingForObviousData) {
    Rng rng(3);
    const auto kind_of = [&](LogicalType type, Shape shape,
                             idx_t n) -> std::optional<EncodingKind> {
        const auto values = IntColumn(rng, type, shape, n, 0.0);
        const auto raw = MakeRawSegment(type, values);
        const auto e = EncodeSegment(*raw, EncodingChoice::Auto);
        if (e == nullptr) {
            return std::nullopt;
        }
        EXPECT_LE(e->MemoryUsage(), raw->MemoryUsage() * 7 / 10 + 64);
        return e->kind();
    };
    // constant data: a single RLE run or per-vector constants, whichever is smaller; both decode to
    // CONSTANT vectors (checked in ConstantAndSingleRunVectorsDecodeToConstantVectors)
    const auto constant_kind = kind_of(LogicalType::Integer(), Shape::Constant, 10000);
    ASSERT_TRUE(constant_kind.has_value());
    EXPECT_TRUE(*constant_kind == EncodingKind::Constant || *constant_kind == EncodingKind::Rle);
    EXPECT_EQ(kind_of(LogicalType::BigInt(), Shape::Ascending, 10000), EncodingKind::Bitpacked);
    EXPECT_EQ(kind_of(LogicalType::Integer(), Shape::TwoValues, 10000), EncodingKind::Bitpacked);
    EXPECT_EQ(kind_of(LogicalType::Date(), Shape::Runs, 10000), EncodingKind::Rle);
    EXPECT_EQ(kind_of(LogicalType::BigInt(), Shape::NarrowAroundHuge, 10000),
              EncodingKind::Bitpacked);
    EXPECT_FALSE(kind_of(LogicalType::BigInt(), Shape::Random, 10000))
        << "random 64-bit values do not compress";
    EXPECT_FALSE(kind_of(LogicalType::BigInt(), Shape::Extremes, 10000))
        << "full-range values do not compress";
}

TEST(IntegerEncodings, DeltaBeatsFrameOfReferenceOnSortedKeys) {
    // l_orderkey-like: ascending with small steps over a large range
    std::vector<Value> values;
    int64_t cur = 1000000;
    Rng rng(4);
    for (int i = 0; i < 20000; i++) {
        cur += static_cast<int64_t>(RandBelow(rng, 4));
        values.push_back(Value::BigInt(cur));
    }
    const auto raw = MakeRawSegment(LogicalType::BigInt(), values);
    const auto e = EncodeSegment(*raw, EncodingChoice::Auto);
    ASSERT_NE(e, nullptr);
    // 2 bits per value plus per-vector headers: far below the ~16 bits that frame-of-reference
    // needs
    EXPECT_LT(e->MemoryUsage(), static_cast<size_t>(20000) * 3 / 8 + 1024);
    ASSERT_NE(RoundTrip(LogicalType::BigInt(), values, EncodingChoice::Auto, "sorted keys"),
              nullptr);
}

TEST(IntegerEncodings, ConstantAndSingleRunVectorsDecodeToConstantVectors) {
    const std::vector<Value> values(6000, Value::Integer(42));
    const auto raw = MakeRawSegment(LogicalType::Integer(), values);
    for (const EncodingChoice c :
         {EncodingChoice::Constant, EncodingChoice::Rle, EncodingChoice::Bitpacked}) {
        const auto seg = WithEncoding(*raw, EncodeSegment(*raw, c));
        Vector v(LogicalType::Integer(), kVectorSize);
        seg->Scan(0, kVectorSize, v);
        EXPECT_EQ(v.format(), VectorFormat::Constant);
        EXPECT_EQ(v.GetValue(1000), Value::Integer(42));
        seg->Scan(4096, 1904, v);
        EXPECT_EQ(v.GetValue(1903), Value::Integer(42));
    }
    // ... but not when the vector has NULLs: those need per-row validity
    std::vector<Value> with_nulls = values;
    with_nulls[5] = Value::Null(LogicalType::Integer());
    const auto raw2 = MakeRawSegment(LogicalType::Integer(), with_nulls);
    const auto seg2 = WithEncoding(*raw2, EncodeSegment(*raw2, EncodingChoice::Constant));
    Vector v(LogicalType::Integer(), kVectorSize);
    seg2->Scan(0, kVectorSize, v);
    EXPECT_EQ(v.format(), VectorFormat::Flat);
    EXPECT_TRUE(v.GetValue(5).IsNull());
    EXPECT_EQ(v.GetValue(6), Value::Integer(42));
}

TEST(IntegerEncodings, ConstantIsOnlyOfferedForConstantData) {
    Rng rng(5);
    EXPECT_EQ(EncodeSegment(
                  *MakeRawSegment(LogicalType::Integer(), IntColumn(rng, LogicalType::Integer(),
                                                                    Shape::Ascending, 3000, 0.0)),
                  EncodingChoice::Constant),
              nullptr);
    EXPECT_NE(EncodeSegment(
                  *MakeRawSegment(LogicalType::Integer(), IntColumn(rng, LogicalType::Integer(),
                                                                    Shape::Constant, 3000, 0.1)),
                  EncodingChoice::Constant),
              nullptr);
}

// ------------------------------------------------------------------------------ doubles

namespace {

std::vector<Value> DoubleColumn(Rng& rng, const std::string& shape, idx_t n, double null_prob) {
    std::vector<double> x(n);
    for (idx_t i = 0; i < n; i++) {
        if (shape == "prices") { // 2 decimal places, like DECIMAL(15,2)
            x[i] = static_cast<double>(RandBelow(rng, 10000000)) / 100.0;
        } else if (shape == "rates") {
            x[i] = static_cast<double>(RandBelow(rng, 11)) / 100.0;
        } else if (shape == "integers") {
            x[i] = static_cast<double>(static_cast<int64_t>(RandBelow(rng, 1000)) - 500);
        } else if (shape == "constant") {
            x[i] = 3.14159;
        } else if (shape == "inexact") { // not representable as n / 10^e for small e
            x[i] = (static_cast<double>(RandBelow(rng, 1000)) + 0.1) + 0.2;
        } else if (shape == "mixed") { // mostly prices, an occasional value that does not scale
            x[i] = Chance(rng, 0.002) ? 0.1 + 0.2
                                      : static_cast<double>(RandBelow(rng, 100000)) / 100.0;
        } else if (shape == "special") {
            const double pool[] = {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(),
                                   -0.0,
                                   0.0,
                                   std::numeric_limits<double>::denorm_min(),
                                   1e300,
                                   -1e300,
                                   9.0e15,
                                   9.1e15,
                                   1.5,
                                   100.0};
            x[i] = pool[RandBelow(rng, std::size(pool))];
        } else if (shape == "huge") {
            x[i] = static_cast<double>(static_cast<int64_t>(rng())) * 1024.0;
        } else if (shape == "negative") {
            x[i] = -static_cast<double>(RandBelow(rng, 100000)) / 1000.0;
        } else {
            x[i] = test::RandomDouble(rng);
        }
    }
    std::vector<Value> out;
    for (idx_t i = 0; i < n; i++) {
        out.push_back(Chance(rng, null_prob) ? Value::Null(LogicalType::Double())
                                             : Value::Double(x[i]));
    }
    return out;
}

} // namespace

TEST(DoubleEncoding, IsLosslessForEveryShapeIncludingNaNInfinityAndNegativeZero) {
    Rng rng(6);
    for (const std::string shape : {"prices", "rates", "integers", "constant", "inexact", "mixed",
                                    "special", "huge", "negative", "random"}) {
        for (const double nulls : {0.0, 0.2, 1.0}) {
            for (const idx_t count : kCounts) {
                const auto values = DoubleColumn(rng, shape, count, nulls);
                for (const EncodingChoice c :
                     {EncodingChoice::Bitpacked, EncodingChoice::Constant, EncodingChoice::Auto}) {
                    RoundTrip(LogicalType::Double(), values, c,
                              "double " + shape + " nulls " + std::to_string(nulls) + " n " +
                                  std::to_string(count));
                    if (::testing::Test::HasFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

TEST(DoubleEncoding, MoneyLikeValuesShrinkAndInexactOnesStayRaw) {
    Rng rng(7);
    const auto size_ratio = [&](const std::string& shape) -> double {
        const auto values = DoubleColumn(rng, shape, 20000, 0.0);
        const auto raw = MakeRawSegment(LogicalType::Double(), values);
        const auto e = EncodeSegment(*raw, EncodingChoice::Auto);
        return e == nullptr ? 1.0
                            : static_cast<double>(e->MemoryUsage()) /
                                  static_cast<double>(raw->MemoryUsage());
    };
    EXPECT_LT(size_ratio("prices"), 0.5) << "0.00..99999.99 needs 24 bits of 64";
    EXPECT_LT(size_ratio("rates"), 0.15) << "0.00..0.10 needs 4 bits";
    EXPECT_LT(size_ratio("integers"), 0.3);
    EXPECT_LT(size_ratio("constant"), 0.01);
    EXPECT_EQ(size_ratio("inexact"), 1.0) << "values that do not scale are not encoded";
    EXPECT_EQ(size_ratio("random"), 1.0);
}

TEST(DoubleEncoding, OneInexactValueOnlyAffectsItsOwnVector) {
    Rng rng(8);
    auto values = DoubleColumn(rng, "prices", 6144, 0.0);
    values[3000] = Value::Double(0.1 + 0.2); // second vector only
    const auto raw = MakeRawSegment(LogicalType::Double(), values);
    const auto e = EncodeSegment(*raw, EncodingChoice::Auto);
    ASSERT_NE(e, nullptr) << "two of the three vectors still compress";
    ASSERT_NE(RoundTrip(LogicalType::Double(), values, EncodingChoice::Auto, "one inexact vector"),
              nullptr);
}

TEST(DoubleEncoding, NegativeZeroIsNotMergedWithZero) {
    std::vector<Value> values;
    for (int i = 0; i < 4096; i++) {
        values.push_back(Value::Double(i % 2 ? -0.0 : 0.0));
    }
    const auto raw = MakeRawSegment(LogicalType::Double(), values);
    const auto seg = WithEncoding(*raw, EncodeSegment(*raw, EncodingChoice::Bitpacked));
    const auto got = ScanAll(*seg);
    for (int i = 0; i < 4096; i++) {
        ASSERT_EQ(std::signbit(got[static_cast<size_t>(i)].GetDouble()), i % 2 == 1) << i;
    }
}

// Offsets of 53 and 54 bits (integer-valued doubles below 9e15 spanning a huge range) cannot take
// the "OR into the mantissa of 2^52" conversion, which is only exact below 2^52.
TEST(DoubleEncoding, OffsetsWiderThan52BitsDecodeExactly) {
    Rng rng(9);
    struct Case {
        double lo, hi; // the offsets need 53, 54, 52 and 53 bits respectively
    };
    for (const Case c :
         {Case{0.0, 8.9e15}, Case{-8.0e15, 8.0e15}, Case{0.0, 4.5e15}, Case{-4.0e15, 4.0e15}}) {
        for (const idx_t count : {idx_t{1}, idx_t{2}, idx_t{70}, kVectorSize, idx_t{5000}}) {
            std::vector<Value> values;
            for (idx_t i = 0; i < count; i++) {
                double d;
                if (i % kVectorSize == 0) {
                    d = c.lo; // every vector spans the whole range
                } else if (i % kVectorSize == 1) {
                    d = c.hi;
                } else {
                    d = std::floor(c.lo + (c.hi - c.lo) *
                                              static_cast<double>(RandBelow(rng, 1000000)) /
                                              1000000.0);
                }
                values.push_back(Chance(rng, 0.05) && i % kVectorSize > 1
                                     ? Value::Null(LogicalType::Double())
                                     : Value::Double(d));
            }
            const std::string what = "range [" + std::to_string(c.lo) + ", " +
                                     std::to_string(c.hi) + "] n " + std::to_string(count);
            const auto e =
                RoundTrip(LogicalType::Double(), values, EncodingChoice::Bitpacked, what);
            ASSERT_NE(e, nullptr) << what;
            EXPECT_EQ(e->kind(), EncodingKind::ScaledDouble) << what;
        }
    }
}

// ------------------------------------------------------------------------------ strings

namespace {

std::vector<Value> StringColumn(Rng& rng, idx_t n, idx_t distinct, double null_prob,
                                bool long_strings) {
    std::vector<std::string> pool;
    for (idx_t d = 0; d < distinct; d++) {
        std::string s = d == 0 ? "" : "v" + std::to_string(d);
        if (long_strings && d % 3 == 0) {
            s += "-a-rather-long-suffix-that-is-out-of-line-" + std::to_string(d);
        }
        pool.push_back(std::move(s));
    }
    std::vector<Value> out;
    for (idx_t i = 0; i < n; i++) {
        out.push_back(Chance(rng, null_prob) ? Value::Null(LogicalType::Varchar())
                                             : Value::Varchar(pool[RandBelow(rng, pool.size())]));
    }
    return out;
}

} // namespace

TEST(DictionaryEncoding, RoundTripsAcrossCardinalitiesNullsAndLongStrings) {
    Rng rng(9);
    for (const idx_t distinct : {1, 2, 7, 100, 1000, 2046, 2047}) {
        for (const double nulls : {0.0, 0.1, 1.0}) {
            for (const bool long_strings : {false, true}) {
                for (const idx_t count : {1, 2048, 2049, 7000}) {
                    const auto values = StringColumn(rng, count, distinct, nulls, long_strings);
                    ASSERT_NE(RoundTrip(LogicalType::Varchar(), values, EncodingChoice::Dictionary,
                                        "distinct " + std::to_string(distinct) + " nulls " +
                                            std::to_string(nulls) + " n " + std::to_string(count)),
                              nullptr);
                }
            }
        }
    }
}

TEST(DictionaryEncoding, TooManyDistinctStringsIsNotEncoded) {
    Rng rng(10);
    std::vector<Value> values;
    for (int i = 0; i < 5000; i++) {
        values.push_back(Value::Varchar("unique-" + std::to_string(i)));
    }
    const auto raw = MakeRawSegment(LogicalType::Varchar(), values);
    EXPECT_EQ(EncodeSegment(*raw, EncodingChoice::Dictionary), nullptr);
    EXPECT_EQ(EncodeSegment(*raw, EncodingChoice::Auto), nullptr);
}

// A dictionary is one vector of kVectorSize entries and must always have room for a NULL entry, so
// 2047 distinct strings is the most it takes - with NULLs or without.
TEST(DictionaryEncoding, TheLimitIs2047DistinctStringsAndIsExact) {
    const auto column = [](idx_t distinct, bool with_nulls) {
        std::vector<Value> values;
        for (idx_t i = 0; i < 2 * kVectorSize + 5; i++) {
            if (with_nulls && i % 7 == 3) {
                values.push_back(Value::Null(LogicalType::Varchar()));
            } else {
                values.push_back(Value::Varchar("s" + std::to_string(i % distinct)));
            }
        }
        return values;
    };
    for (const bool with_nulls : {false, true}) {
        const std::string what = with_nulls ? " with NULLs" : " without NULLs";
        const auto fits = column(kVectorSize - 1, with_nulls);
        ASSERT_NE(RoundTrip(LogicalType::Varchar(), fits, EncodingChoice::Dictionary,
                            "2047 distinct" + what),
                  nullptr);
        for (const idx_t distinct : {kVectorSize, kVectorSize + 1}) {
            const auto raw = MakeRawSegment(LogicalType::Varchar(), column(distinct, with_nulls));
            EXPECT_EQ(EncodeSegment(*raw, EncodingChoice::Dictionary), nullptr)
                << distinct << " distinct" << what;
        }
    }
}

TEST(DictionaryEncoding, ScansHandBackDictionaryVectorsWithoutCopyingStrings) {
    Rng rng(11);
    const auto values = StringColumn(rng, 4096, 7, 0.1, true);
    const auto raw = MakeRawSegment(LogicalType::Varchar(), values);
    const auto seg = WithEncoding(*raw, EncodeSegment(*raw, EncodingChoice::Auto));
    ASSERT_TRUE(seg->encoded());
    EXPECT_EQ(seg->encoding()->kind(), EncodingKind::Dictionary);
    Vector a(LogicalType::Varchar(), kVectorSize), b(LogicalType::Varchar(), kVectorSize);
    seg->Scan(0, kVectorSize, a);
    seg->Scan(2048, kVectorSize, b);
    EXPECT_EQ(a.format(), VectorFormat::Dictionary);
    // both vectors share one dictionary: the same string bytes serve every row of the segment
    EXPECT_EQ(&a.DictionaryChild().FlatData<string_t>()[0] ==
                  &b.DictionaryChild().FlatData<string_t>()[0],
              true);
    // a vector that outlives the segment keeps the dictionary (and its strings) alive
    Vector kept(LogicalType::Varchar(), kVectorSize);
    {
        const auto temp = WithEncoding(*raw, EncodeSegment(*raw, EncodingChoice::Dictionary));
        temp->Scan(0, kVectorSize, kept);
    }
    for (idx_t i = 0; i < kVectorSize; i++) {
        ASSERT_TRUE(BitIdentical(kept.GetValue(i), values[i])) << i;
    }
}

TEST(DictionaryEncoding, SmallDictionariesShrinkTheSegment) {
    Rng rng(12);
    const auto values = StringColumn(rng, 50000, 7, 0.0, false); // like l_shipmode
    const auto raw = MakeRawSegment(LogicalType::Varchar(), values);
    const auto e = EncodeSegment(*raw, EncodingChoice::Auto);
    ASSERT_NE(e, nullptr);
    EXPECT_LT(e->MemoryUsage() * 4, raw->MemoryUsage())
        << "3 bits per row against 16 bytes per string_t";
}

// ------------------------------------------------------------------------------ everything
// together

TEST(Encodings, RandomColumnsRoundTripWhateverTheChooserDecides) {
    Rng rng(13);
    for (int iter = 0; iter < 250; iter++) {
        const idx_t count = 1 + RandBelow(rng, 9000);
        const double nulls = Chance(rng, 0.5) ? 0.0 : (Chance(rng, 0.2) ? 0.95 : 0.15);
        switch (RandBelow(rng, 3)) {
        case 0: {
            const LogicalType types[] = {LogicalType::Boolean(), LogicalType::Integer(),
                                         LogicalType::BigInt(), LogicalType::Date()};
            const LogicalType type = types[RandBelow(rng, 4)];
            const auto values =
                IntColumn(rng, type, static_cast<Shape>(RandBelow(rng, 9)), count, nulls);
            RoundTrip(type, values, EncodingChoice::Auto, "random int " + std::to_string(iter));
            break;
        }
        case 1: {
            static const char* const shapes[] = {"prices",  "rates", "integers", "constant",
                                                 "inexact", "mixed", "special",  "negative"};
            RoundTrip(LogicalType::Double(),
                      DoubleColumn(rng, shapes[RandBelow(rng, 8)], count, nulls),
                      EncodingChoice::Auto, "random double " + std::to_string(iter));
            break;
        }
        default:
            RoundTrip(LogicalType::Varchar(),
                      StringColumn(rng, count, 1 + RandBelow(rng, 600), nulls, Chance(rng, 0.5)),
                      EncodingChoice::Auto, "random string " + std::to_string(iter));
            break;
        }
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

TEST(Encodings, EncodedSegmentsAreSafeToScanFromManyThreads) {
    Rng rng(14);
    const auto ints = IntColumn(rng, LogicalType::Integer(), Shape::Ascending, 40000, 0.1);
    const auto doubles = DoubleColumn(rng, "prices", 40000, 0.1);
    const auto strings = StringColumn(rng, 40000, 9, 0.1, true);
    std::vector<std::shared_ptr<ColumnSegment>> segments;
    std::vector<const std::vector<Value>*> expected = {&ints, &doubles, &strings};
    const LogicalType types[] = {LogicalType::Integer(), LogicalType::Double(),
                                 LogicalType::Varchar()};
    for (int i = 0; i < 3; i++) {
        const auto raw = MakeRawSegment(types[i], *expected[static_cast<size_t>(i)]);
        auto e = EncodeSegment(*raw, EncodingChoice::Auto);
        ASSERT_NE(e, nullptr);
        segments.push_back(WithEncoding(*raw, std::move(e)));
    }
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < 6; t++) {
        threads.emplace_back([&, t] {
            Rng order(static_cast<uint64_t>(t));
            for (int round = 0; round < 3; round++) {
                for (size_t s = 0; s < segments.size(); s++) {
                    const auto got = ScanAll(*segments[s], &order);
                    for (idx_t i = 0; i < got.size(); i++) {
                        if (!BitIdentical(got[i], (*expected[s])[i])) {
                            failures++;
                            return;
                        }
                    }
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

TEST(Encodings, TurningCompressionOffKeepsEverySegmentRaw) {
    Rng rng(15);
    const auto values = IntColumn(rng, LogicalType::Integer(), Shape::Constant, 5000, 0.0);
    {
        const test::ScopedCompression off(false);
        ColumnBuilder b(LogicalType::Integer(), 8192);
        Vector src(LogicalType::Integer(), kVectorSize);
        for (idx_t i = 0; i < 2048; i++) {
            src.SetValue(i, values[i]);
        }
        b.Append(src, 0, 2048);
        EXPECT_FALSE(b.Seal()->encoded());
    }
    {
        const test::ScopedCompression on(true);
        ColumnBuilder b(LogicalType::Integer(), 8192);
        Vector src(LogicalType::Integer(), kVectorSize);
        for (idx_t i = 0; i < 2048; i++) {
            src.SetValue(i, values[i]);
        }
        b.Append(src, 0, 2048);
        const auto seg = b.Seal();
        EXPECT_TRUE(seg->encoded());
        EXPECT_LT(seg->MemoryUsage(), 2048 * 4 / 2);
    }
}

} // namespace cdb
