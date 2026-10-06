// Serialization of column data: values, chunks of rows (the write-ahead log) and column segments in
// every stored encoding (the checkpoint file). Round trips must be bit-exact; every damaged input
// must either be rejected with a Corruption error or decode into something that is safe to scan -
// the tests that flip bytes are meant to be run under ASan/UBSan.

#include "storage/segment_io.h"

#include "storage/encoding.h"
#include "storage_test_util.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cdb {

namespace {

using test::BitIdentical;
using test::Chance;
using test::RandBelow;
using test::Rng;

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

std::vector<Value> ScanAll(const ColumnSegment& seg) {
    std::vector<Value> out;
    Vector v(seg.type(), kVectorSize);
    for (idx_t at = 0; at < seg.count(); at += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, seg.count() - at);
        seg.Scan(at, n, v);
        v.Verify(n);
        for (idx_t i = 0; i < n; i++) {
            out.push_back(v.GetValue(i));
        }
    }
    return out;
}

bool SameOptional(const std::optional<Value>& a, const std::optional<Value>& b) {
    return a.has_value() == b.has_value() && (!a.has_value() || BitIdentical(*a, *b));
}

std::vector<uint8_t> Serialize(const ColumnSegment& seg) {
    BinaryWriter w;
    WriteSegment(w, seg);
    return w.Take();
}

std::shared_ptr<ColumnSegment> Deserialize(const std::vector<uint8_t>& bytes, LogicalType type,
                                           idx_t count, bool expect_all_consumed = true) {
    BinaryReader r(bytes.data(), bytes.size(), "segment");
    auto seg = ReadSegment(r, type, count);
    if (expect_all_consumed) {
        r.ExpectEnd();
    }
    return seg;
}

void ExpectSameSegment(const ColumnSegment& got, const ColumnSegment& want,
                       const std::string& what) {
    ASSERT_EQ(got.type(), want.type()) << what;
    ASSERT_EQ(got.count(), want.count()) << what;
    EXPECT_EQ(got.stats().count, want.stats().count) << what;
    EXPECT_EQ(got.stats().null_count, want.stats().null_count) << what;
    EXPECT_TRUE(SameOptional(got.stats().min, want.stats().min)) << what << ": min";
    EXPECT_TRUE(SameOptional(got.stats().max, want.stats().max)) << what << ": max";
    EXPECT_EQ(got.encoded(), want.encoded()) << what;
    if (got.encoded() && want.encoded()) {
        EXPECT_EQ(got.encoding()->kind(), want.encoding()->kind()) << what;
    }
    const std::vector<Value> a = ScanAll(got), b = ScanAll(want);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); i++) {
        ASSERT_TRUE(BitIdentical(a[i], b[i]))
            << what << ": row " << i << " got " << a[i].ToString() << " want " << b[i].ToString();
    }
}

void ExpectCorruption(const std::function<void()>& fn, const std::string& what) {
    try {
        fn();
        ADD_FAILURE() << what << ": expected a Corruption error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Corruption) << what << ": " << e.what();
    }
}

// ---------------------------------------------------------------------------------- data

enum class Shape { Constant, Runs, Ascending, SmallDomain, Random };
constexpr Shape kShapes[] = {Shape::Constant, Shape::Runs, Shape::Ascending, Shape::SmallDomain,
                             Shape::Random};

std::vector<Value> MakeColumn(LogicalType type, Shape shape, idx_t n, double nulls, Rng& rng) {
    std::vector<Value> out;
    Value last = test::RandomValue(rng, type, 0.0);
    int64_t counter = static_cast<int64_t>(RandBelow(rng, 1000));
    std::vector<Value> domain;
    for (int i = 0; i < 5; i++) {
        domain.push_back(test::RandomValue(rng, type, 0.0));
    }
    for (idx_t i = 0; i < n; i++) {
        if (Chance(rng, nulls)) {
            out.push_back(Value::Null(type));
            continue;
        }
        switch (shape) {
        case Shape::Constant:
            out.push_back(last);
            break;
        case Shape::Runs:
            if (Chance(rng, 0.02)) {
                last = test::RandomValue(rng, type, 0.0);
            }
            out.push_back(last);
            break;
        case Shape::Ascending:
            counter += static_cast<int64_t>(RandBelow(rng, 7));
            switch (type.id()) {
            case TypeId::Boolean:
                out.push_back(Value::Boolean(counter % 2 == 0));
                break;
            case TypeId::Integer:
                out.push_back(Value::Integer(static_cast<int32_t>(counter)));
                break;
            case TypeId::Date:
                out.push_back(Value::Date(date_t{static_cast<int32_t>(counter)}));
                break;
            case TypeId::BigInt:
                out.push_back(Value::BigInt(counter));
                break;
            case TypeId::Double: // money-like: exactly representable hundredths
                out.push_back(Value::Double(static_cast<double>(counter) / 100.0));
                break;
            case TypeId::Varchar:
                out.push_back(Value::Varchar("key" + std::to_string(counter % 40)));
                break;
            }
            break;
        case Shape::SmallDomain:
            out.push_back(domain[RandBelow(rng, domain.size())]);
            break;
        case Shape::Random:
            out.push_back(test::RandomValue(rng, type, 0.0));
            break;
        }
    }
    return out;
}

constexpr idx_t kLengths[] = {1, 2, 63, 2047, 2048, 2049, 4097};
constexpr EncodingChoice kChoices[] = {EncodingChoice::Auto, EncodingChoice::Constant,
                                       EncodingChoice::Rle, EncodingChoice::Bitpacked,
                                       EncodingChoice::Dictionary};

} // namespace

// ---------------------------------------------------------------------------------- values

TEST(ValueIo, EveryTypeRoundTripsIncludingTheAwkwardOnes) {
    const std::vector<Value> values = {
        Value::Boolean(true),
        Value::Boolean(false),
        Value::Integer(std::numeric_limits<int32_t>::min()),
        Value::Integer(-1),
        Value::BigInt(std::numeric_limits<int64_t>::max()),
        Value::Double(-0.0),
        Value::Double(std::numeric_limits<double>::quiet_NaN()),
        Value::Double(std::numeric_limits<double>::infinity()),
        Value::Double(std::numeric_limits<double>::denorm_min()),
        Value::Date(date_t{-719162}),
        Value::Date(date_t{2932896}),
        Value::Varchar(""),
        Value::Varchar(std::string("a\0b\xff", 4)),
        Value::Varchar(std::string(500, 'x')),
    };
    for (const Value& v : values) {
        BinaryWriter w;
        WriteValue(w, v);
        BinaryReader r(w.buffer().data(), w.buffer().size(), "value");
        const Value back = ReadValue(r, v.type());
        EXPECT_TRUE(BitIdentical(back, v)) << v.ToString();
        r.ExpectEnd();
        // every truncation is rejected
        for (size_t cut = 0; cut < w.size(); cut++) {
            ExpectCorruption(
                [&] {
                    BinaryReader t(w.buffer().data(), cut, "value");
                    ReadValue(t, v.type());
                },
                "truncated value");
        }
    }
    BinaryWriter bad;
    bad.U8(7);
    BinaryReader r(bad.buffer().data(), 1, "value");
    ExpectCorruption([&] { ReadValue(r, LogicalType::Boolean()); }, "BOOLEAN 7");
}

// ---------------------------------------------------------------------------------- chunks

TEST(ChunkIo, ChunksOfEveryTypeAndFormatRoundTrip) {
    Rng rng(1);
    for (const idx_t n :
         {idx_t{0}, idx_t{1}, idx_t{2}, idx_t{63}, idx_t{64}, idx_t{2047}, idx_t{2048}}) {
        for (const double nulls : {0.0, 0.3, 1.0}) {
            const std::vector<LogicalType> types = test::AllTypes();
            DataChunk chunk;
            chunk.Initialize(types, kVectorSize);
            std::vector<std::vector<Value>> rows(types.size());
            for (idx_t c = 0; c < types.size(); c++) {
                for (idx_t i = 0; i < n; i++) {
                    const Value v = test::RandomValue(rng, types[c], nulls);
                    chunk.column(c).SetValue(i, v);
                    rows[c].push_back(v);
                }
            }
            chunk.SetCardinality(n);
            BinaryWriter w;
            WriteChunk(w, chunk);
            DataChunk back;
            BinaryReader r(w.buffer().data(), w.buffer().size(), "chunk");
            ReadChunk(r, types, back);
            r.ExpectEnd();
            ASSERT_EQ(back.size(), n);
            back.Verify();
            for (idx_t c = 0; c < types.size(); c++) {
                for (idx_t i = 0; i < n; i++) {
                    ASSERT_TRUE(BitIdentical(back.GetValue(c, i), rows[c][i]))
                        << "n " << n << " nulls " << nulls << " column " << c << " row " << i;
                }
            }
        }
    }
}

TEST(ChunkIo, ConstantDictionaryAndSlicedVectorsAreWrittenByTheirLogicalRows) {
    // a constant vector, a dictionary vector (selection into a child), and a flat vector, then the
    // same chunk sliced to a reversed subset of rows
    const std::vector<LogicalType> types = {LogicalType::Integer(), LogicalType::Varchar(),
                                            LogicalType::Double()};
    SelectionVector pick(kVectorSize);
    for (idx_t i = 0; i < 40; i++) {
        pick.Set(i, static_cast<sel_t>(99 - 2 * i));
    }
    for (const bool sliced : {false, true}) {
        DataChunk c;
        c.Initialize(types, kVectorSize);
        c.column(0).SetConstant(Value::Integer(42));
        Vector child(LogicalType::Varchar(), kVectorSize);
        for (idx_t i = 0; i < 10; i++) {
            child.SetValue(i, i == 3 ? Value::Null(LogicalType::Varchar())
                                     : Value::Varchar("a dictionary entry longer than twelve " +
                                                      std::to_string(i)));
        }
        SelectionVector codes(kVectorSize);
        for (idx_t i = 0; i < 100; i++) {
            codes.Set(i, static_cast<sel_t>(i % 10));
        }
        c.column(1).SetDictionary(child, std::move(codes));
        for (idx_t i = 0; i < 100; i++) {
            c.column(2).SetValue(i, Value::Double(static_cast<double>(i)));
        }
        c.SetCardinality(100);
        if (sliced) {
            c.Slice(pick, 40);
        }
        BinaryWriter w;
        WriteChunk(w, c);
        DataChunk back;
        BinaryReader r(w.buffer().data(), w.buffer().size(), "chunk");
        ReadChunk(r, types, back);
        r.ExpectEnd();
        ASSERT_EQ(back.size(), c.size());
        for (idx_t col = 0; col < 3; col++) {
            for (idx_t i = 0; i < c.size(); i++) {
                ASSERT_TRUE(BitIdentical(back.GetValue(col, i), c.GetValue(col, i)))
                    << (sliced ? "sliced " : "") << "column " << col << " row " << i;
            }
        }
    }
}

TEST(ChunkIo, NullsCostBitsAndTheirGarbageValuesAreNotWritten) {
    // two chunks that differ only in what sits under their NULLs serialize identically
    const std::vector<LogicalType> types = {LogicalType::BigInt()};
    std::vector<uint8_t> first, second;
    for (const int64_t filler : {int64_t{0}, int64_t{0x1234567890ABCDEF}}) {
        DataChunk chunk;
        chunk.Initialize(types, kVectorSize);
        for (idx_t i = 0; i < 100; i++) {
            if (i % 3 == 0) {
                chunk.column(0).FlatData<int64_t>()[i] = filler;
                chunk.column(0).Validity().SetInvalid(i);
            } else {
                chunk.column(0).SetValue(i, Value::BigInt(static_cast<int64_t>(i)));
            }
        }
        chunk.SetCardinality(100);
        BinaryWriter w;
        WriteChunk(w, chunk);
        (filler == 0 ? first : second) = w.Take();
    }
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.size(), 4 + 1 + 13 + 66 * 8) << "u32 count, flag, 13 bitmap bytes, 66 values";
}

TEST(ChunkIo, ARowCountAboveTheVectorSizeIsRejected) {
    BinaryWriter w;
    w.U32(kVectorSize + 1);
    DataChunk out;
    BinaryReader r(w.buffer().data(), w.buffer().size(), "chunk");
    ExpectCorruption([&] { ReadChunk(r, {LogicalType::Integer()}, out); }, "oversized chunk");
}

TEST(ChunkIo, EveryTruncationIsRejectedAndEveryByteChangeIsSafe) {
    Rng rng(3);
    const std::vector<LogicalType> types = test::AllTypes();
    DataChunk chunk;
    chunk.Initialize(types, kVectorSize);
    for (idx_t c = 0; c < types.size(); c++) {
        for (idx_t i = 0; i < 20; i++) {
            chunk.column(c).SetValue(i, test::RandomValue(rng, types[c], 0.25));
        }
    }
    chunk.SetCardinality(20);
    BinaryWriter w;
    WriteChunk(w, chunk);
    const std::vector<uint8_t> bytes = w.buffer();
    for (size_t cut = 0; cut < bytes.size(); cut++) {
        ExpectCorruption(
            [&] {
                DataChunk out;
                BinaryReader r(bytes.data(), cut, "chunk");
                ReadChunk(r, types, out);
            },
            "chunk cut at " + std::to_string(cut));
    }
    size_t rejected = 0, accepted = 0;
    for (size_t at = 0; at < bytes.size(); at++) {
        for (const uint8_t flip : {uint8_t{0x01}, uint8_t{0x80}, uint8_t{0xFF}}) {
            std::vector<uint8_t> damaged = bytes;
            damaged[at] ^= flip;
            try {
                DataChunk out;
                BinaryReader r(damaged.data(), damaged.size(), "chunk");
                ReadChunk(r, types, out);
                out.Verify();
                for (idx_t c = 0; c < types.size(); c++) {
                    for (idx_t i = 0; i < out.size(); i++) {
                        out.GetValue(c, i);
                    }
                }
                accepted++;
            } catch (const Error& e) {
                EXPECT_EQ(e.code(), ErrorCode::Corruption) << e.what();
                rejected++;
            }
        }
    }
    EXPECT_GT(rejected, 0U);
    EXPECT_GT(accepted, 0U) << "value bytes have no checksum at this level, so some changes decode";
}

// ---------------------------------------------------------------------------------- segments

TEST(SegmentIo, RawSegmentsOfEveryTypeRoundTrip) {
    Rng rng(4);
    for (const LogicalType type : test::AllTypes()) {
        for (const idx_t n : kLengths) {
            for (const double nulls : {0.0, 0.25, 1.0}) {
                for (const Shape shape : kShapes) {
                    const std::vector<Value> values = MakeColumn(type, shape, n, nulls, rng);
                    const auto raw = MakeRawSegment(type, values);
                    ASSERT_FALSE(raw->encoded());
                    const std::vector<uint8_t> bytes = Serialize(*raw);
                    const auto back = Deserialize(bytes, type, n);
                    ExpectSameSegment(*back, *raw,
                                      type.ToString() + " n=" + std::to_string(n) +
                                          " nulls=" + std::to_string(nulls));
                    if (::testing::Test::HasFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

TEST(SegmentIo, EncodedSegmentsRoundTripInEveryEncoding) {
    Rng rng(5);
    size_t per_kind[6] = {};
    for (const LogicalType type : test::AllTypes()) {
        for (const idx_t n : kLengths) {
            for (const double nulls : {0.0, 0.2}) {
                for (const Shape shape : kShapes) {
                    const std::vector<Value> values = MakeColumn(type, shape, n, nulls, rng);
                    const auto raw = MakeRawSegment(type, values);
                    for (const EncodingChoice choice : kChoices) {
                        auto encoded = EncodeSegment(*raw, choice);
                        if (encoded == nullptr) {
                            continue;
                        }
                        const auto seg = WithEncoding(*raw, encoded);
                        per_kind[static_cast<int>(encoded->kind())]++;
                        const std::vector<uint8_t> bytes = Serialize(*seg);
                        const auto back = Deserialize(bytes, type, n);
                        ExpectSameSegment(*back, *seg,
                                          type.ToString() + " " + EncodingName(encoded->kind()) +
                                              " n=" + std::to_string(n));
                        if (::testing::Test::HasFailure()) {
                            return;
                        }
                        // writing what was read gives the same bytes
                        EXPECT_EQ(Serialize(*back), bytes) << "serialization is canonical";
                    }
                }
            }
        }
    }
    for (const EncodingKind kind :
         {EncodingKind::Constant, EncodingKind::Rle, EncodingKind::Bitpacked,
          EncodingKind::ScaledDouble, EncodingKind::Dictionary}) {
        EXPECT_GT(per_kind[static_cast<int>(kind)], 0U)
            << EncodingName(kind) << " was never exercised";
    }
}

TEST(SegmentIo, TheStatisticsAreTheOnesTheSegmentHad) {
    Rng rng(6);
    for (const LogicalType type : test::AllTypes()) {
        const std::vector<Value> values = MakeColumn(type, Shape::Random, 3000, 0.1, rng);
        const auto raw = MakeRawSegment(type, values);
        const auto back = Deserialize(Serialize(*raw), type, 3000);
        // recomputed from the decoded rows they must agree too
        const ColumnStats recomputed =
            ComputeColumnStats(type, back->raw_data(), back->validity(), back->count());
        EXPECT_EQ(recomputed.null_count, back->stats().null_count);
        EXPECT_TRUE(SameOptional(recomputed.min, back->stats().min));
        EXPECT_TRUE(SameOptional(recomputed.max, back->stats().max));
    }
}

TEST(SegmentIo, AllNullAndLongStringSegmentsKeepTheirBoundsFlags) {
    // all NULL: no bounds; strings longer than the zone-map limit: no bounds
    const auto all_null = MakeRawSegment(
        LogicalType::Integer(), std::vector<Value>(100, Value::Null(LogicalType::Integer())));
    const auto back = Deserialize(Serialize(*all_null), LogicalType::Integer(), 100);
    EXPECT_FALSE(back->stats().min.has_value());
    EXPECT_EQ(back->stats().null_count, 100U);
    const auto longs =
        MakeRawSegment(LogicalType::Varchar(), {Value::Varchar(std::string(100, 'a')),
                                                Value::Varchar("b"), Value::Varchar("c")});
    EXPECT_FALSE(longs->stats().min.has_value());
    const auto longs_back = Deserialize(Serialize(*longs), LogicalType::Varchar(), 3);
    EXPECT_FALSE(longs_back->stats().min.has_value());
    EXPECT_EQ(ScanAll(*longs_back)[0].GetVarchar(), std::string(100, 'a'));
}

TEST(SegmentIo, TheExpectedTypeAndCountAreEnforced) {
    Rng rng(7);
    const auto raw = MakeRawSegment(
        LogicalType::Integer(), MakeColumn(LogicalType::Integer(), Shape::Random, 100, 0.0, rng));
    const std::vector<uint8_t> bytes = Serialize(*raw);
    ExpectCorruption([&] { Deserialize(bytes, LogicalType::BigInt(), 100); }, "wrong type");
    ExpectCorruption([&] { Deserialize(bytes, LogicalType::Integer(), 99); }, "wrong count");
    ExpectCorruption([&] { Deserialize(bytes, LogicalType::Integer(), 101); }, "wrong count");
    ExpectCorruption([&] { Deserialize(bytes, LogicalType::Integer(), 0); }, "zero rows");
}

namespace {

// Every segment shape worth damaging: raw and each encoding, small enough to try every byte.
std::vector<std::pair<std::string, std::shared_ptr<ColumnSegment>>> SmallSegments() {
    Rng rng(8);
    std::vector<std::pair<std::string, std::shared_ptr<ColumnSegment>>> out;
    const auto add = [&](const std::string& name, LogicalType type, Shape shape, idx_t n,
                         EncodingChoice choice) {
        const auto raw = MakeRawSegment(type, MakeColumn(type, shape, n, 0.2, rng));
        auto encoded = EncodeSegment(*raw, choice);
        out.emplace_back(name, encoded != nullptr ? WithEncoding(*raw, encoded) : raw);
    };
    add("raw int", LogicalType::Integer(), Shape::Random, 40, EncodingChoice::Auto);
    add("raw string", LogicalType::Varchar(), Shape::Random, 25, EncodingChoice::Auto);
    add("raw double", LogicalType::Double(), Shape::Random, 25, EncodingChoice::Auto);
    add("bitpacked", LogicalType::BigInt(), Shape::Ascending, 60, EncodingChoice::Bitpacked);
    add("constant", LogicalType::Integer(), Shape::Constant, 50, EncodingChoice::Constant);
    add("rle", LogicalType::Integer(), Shape::Runs, 80, EncodingChoice::Rle);
    add("rle bool", LogicalType::Boolean(), Shape::Runs, 50, EncodingChoice::Rle);
    add("scaled double", LogicalType::Double(), Shape::Ascending, 50, EncodingChoice::Bitpacked);
    add("dictionary", LogicalType::Varchar(), Shape::SmallDomain, 60, EncodingChoice::Dictionary);
    add("dictionary all null", LogicalType::Varchar(), Shape::Random, 10,
        EncodingChoice::Dictionary);
    return out;
}

} // namespace

TEST(SegmentIo, EveryTruncationOfASegmentIsRejected) {
    for (const auto& [name, seg] : SmallSegments()) {
        const std::vector<uint8_t> bytes = Serialize(*seg);
        for (size_t cut = 0; cut < bytes.size(); cut++) {
            ExpectCorruption(
                [&] {
                    const std::vector<uint8_t> prefix(bytes.begin(),
                                                      bytes.begin() + static_cast<long>(cut));
                    Deserialize(prefix, seg->type(), seg->count());
                },
                name + " cut at " + std::to_string(cut));
        }
        ExpectCorruption(
            [&] {
                std::vector<uint8_t> longer = bytes;
                longer.push_back(0);
                Deserialize(longer, seg->type(), seg->count());
            },
            name + " with a trailing byte");
    }
}

TEST(SegmentIo, ChangingAnyByteToAnyValueNeverMakesTheSegmentUnsafeToScan) {
    // Without a checksum at this level some changes are simply different data; what must hold is
    // that anything accepted can be scanned end to end (run under ASan/UBSan: no out-of-bounds
    // read, no invalid enum, no huge allocation) and anything else is a Corruption error.
    size_t rejected = 0, accepted = 0;
    for (const auto& [name, seg] : SmallSegments()) {
        const std::vector<uint8_t> bytes = Serialize(*seg);
        for (size_t at = 0; at < bytes.size(); at++) {
            // the values that matter to lengths, counts, widths and flags (0, 1, powers of two and
            // their neighbours, all ones) and two flips of the original byte; trying all 255 values
            // of every byte was exhaustive but took a minute
            for (const int value : {0, 1, 2, 3, 7, 8, 15, 16, 63, 64, 65, 127, 128, 255,
                                    bytes[at] ^ 0x01, bytes[at] ^ 0x80}) {
                if (bytes[at] == value) {
                    continue;
                }
                std::vector<uint8_t> damaged = bytes;
                damaged[at] = static_cast<uint8_t>(value);
                try {
                    const auto back = Deserialize(damaged, seg->type(), seg->count());
                    ScanAll(*back);
                    accepted++;
                } catch (const Error& e) {
                    ASSERT_EQ(e.code(), ErrorCode::Corruption)
                        << name << " byte " << at << " -> " << value << ": " << e.what();
                    rejected++;
                }
            }
        }
    }
    EXPECT_GT(rejected, 1000U);
    EXPECT_GT(accepted, 100U);
}

TEST(SegmentIo, AHostileStatisticsBlockIsRejected) {
    // null_count above the row count, minimum above maximum, and statistics that contradict the
    // validity bits
    // craft by hand: statistics claiming 0 NULLs while the validity bitmap has some
    BinaryWriter w;
    w.U8(static_cast<uint8_t>(TypeId::Integer));
    w.U8(0);
    w.U32(4);
    w.U32(0); // null_count
    w.U8(0);  // no bounds
    w.U8(0);  // no distinct-value sketch
    w.U8(1);  // has nulls
    w.U8(0b0101);
    for (int i = 0; i < 2; i++) {
        w.U32(7);
    }
    ExpectCorruption(
        [&] {
            BinaryReader r(w.buffer().data(), w.buffer().size(), "segment");
            ReadSegment(r, LogicalType::Integer(), 4);
        },
        "statistics that disagree with the validity bits");
    BinaryWriter minmax;
    minmax.U8(static_cast<uint8_t>(TypeId::Integer));
    minmax.U8(0);
    minmax.U32(1);
    minmax.U32(0);
    minmax.U8(1);
    minmax.U32(9); // min
    minmax.U32(3); // max < min
    minmax.U8(0);  // no distinct-value sketch
    minmax.U8(0);
    minmax.U32(5);
    ExpectCorruption(
        [&] {
            BinaryReader r(minmax.buffer().data(), minmax.buffer().size(), "segment");
            ReadSegment(r, LogicalType::Integer(), 1);
        },
        "minimum above maximum");
}

// ---------------------------------------------------------------------------------- sketches

TEST(SegmentIo, TheDistinctSketchRoundTripsInBothFormsAndAnAbsentOneStaysAbsent) {
    Rng rng(31);
    // a low-cardinality column is stored sparsely, a high-cardinality one densely
    const auto few =
        MakeRawSegment(LogicalType::Integer(),
                       MakeColumn(LogicalType::Integer(), Shape::SmallDomain, 3000, 0.1, rng));
    const auto many = MakeRawSegment(
        LogicalType::BigInt(), MakeColumn(LogicalType::BigInt(), Shape::Random, 3000, 0.1, rng));
    for (const auto& seg : {few, many}) {
        ASSERT_NE(seg->stats().distinct, nullptr) << "a sealed segment carries a sketch";
        const auto back = Deserialize(Serialize(*seg), seg->type(), seg->count());
        ASSERT_NE(back->stats().distinct, nullptr);
        EXPECT_TRUE(*back->stats().distinct == *seg->stats().distinct);
    }
    EXPECT_LT(few->stats().distinct->NonZeroRegisters() * 3 + 2, HyperLogLog::kRegisters);
    EXPECT_GE(many->stats().distinct->NonZeroRegisters() * 3 + 2, HyperLogLog::kRegisters);
    EXPECT_LT(Serialize(*few).size(), Serialize(*many).size() / 2) << "the sparse form is smaller";

    // an encoded segment keeps the sketch of its rows
    const auto raw =
        MakeRawSegment(LogicalType::Varchar(),
                       MakeColumn(LogicalType::Varchar(), Shape::SmallDomain, 200, 0.0, rng));
    const auto encoded = WithEncoding(*raw, EncodeSegment(*raw, EncodingChoice::Dictionary));
    const auto encoded_back = Deserialize(Serialize(*encoded), LogicalType::Varchar(), 200);
    ASSERT_NE(encoded_back->stats().distinct, nullptr);
    EXPECT_TRUE(*encoded_back->stats().distinct == *raw->stats().distinct);

    // the frozen tail of a builder has none, and still has none after a round trip
    ColumnBuilder builder(LogicalType::Integer(), kVectorSize);
    Vector src(LogicalType::Integer(), kVectorSize);
    src.SetValue(0, Value::Integer(7));
    builder.Append(src, 0, 1);
    const auto tail = builder.Snapshot();
    EXPECT_EQ(tail->stats().distinct, nullptr);
    const auto tail_back = Deserialize(Serialize(*tail), LogicalType::Integer(), 1);
    EXPECT_EQ(tail_back->stats().distinct, nullptr);
}

namespace {

// A raw INTEGER segment block up to its distinct-value sketch (the rest is not reached by the
// hostile cases below).
BinaryWriter SketchBlock(idx_t count, idx_t null_count) {
    BinaryWriter w;
    w.U8(static_cast<uint8_t>(TypeId::Integer));
    w.U8(0);
    w.U32(static_cast<uint32_t>(count));
    w.U32(static_cast<uint32_t>(null_count));
    w.U8(0); // no bounds
    return w;
}

void ExpectSketchRejected(const BinaryWriter& w, idx_t count, const std::string& what,
                          const std::string& message_part) {
    try {
        BinaryReader r(w.buffer().data(), w.buffer().size(), "segment");
        ReadSegment(r, LogicalType::Integer(), count);
        ADD_FAILURE() << what << ": accepted";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Corruption) << what;
        EXPECT_NE(std::string(e.what()).find(message_part), std::string::npos)
            << what << ": " << e.what();
    }
}

} // namespace

TEST(SegmentIo, AHostileDistinctSketchIsRejectedForTheRightReason) {
    {
        BinaryWriter w = SketchBlock(4, 0);
        w.U8(3); // no such form
        ExpectSketchRejected(w, 4, "unknown form", "sketch form");
    }
    {
        BinaryWriter w = SketchBlock(4, 0);
        w.U8(1); // sparse
        w.U8(200);
        w.U8(0); // 200 entries claimed, none present
        ExpectSketchRejected(w, 4, "count beyond the data", "registers in");
    }
    for (const auto& [index, value, name] :
         {std::tuple<int, int, const char*>{4096, 3, "index out of range"},
          {7, 0, "a zero register"},
          {7, 200, "a register above the maximum"}}) {
        BinaryWriter w = SketchBlock(4, 0);
        w.U8(1);
        w.U8(1);
        w.U8(0);
        w.U8(static_cast<uint8_t>(index & 0xff));
        w.U8(static_cast<uint8_t>(index >> 8));
        w.U8(static_cast<uint8_t>(value));
        ExpectSketchRejected(w, 4, name, "sketch");
    }
    {
        BinaryWriter w = SketchBlock(4, 0);
        w.U8(1); // sparse with indexes that do not increase
        w.U8(2);
        w.U8(0);
        for (const int index : {9, 9}) {
            w.U8(static_cast<uint8_t>(index));
            w.U8(0);
            w.U8(2);
        }
        ExpectSketchRejected(w, 4, "repeated index", "malformed sparse");
    }
    {
        BinaryWriter w = SketchBlock(4, 0);
        w.U8(2); // dense with a register above the maximum
        std::vector<uint8_t> registers(HyperLogLog::kRegisters, 1);
        registers[17] = HyperLogLog::kMaxRegister + 1;
        w.Bytes(registers.data(), registers.size());
        ExpectSketchRejected(w, 4, "dense out of range", "out-of-range register");
    }
    {
        BinaryWriter w = SketchBlock(4, 0);
        w.U8(2); // dense, but cut short
        const std::vector<uint8_t> registers(100, 1);
        w.Bytes(registers.data(), registers.size());
        ExpectCorruption(
            [&] {
                BinaryReader r(w.buffer().data(), w.buffer().size(), "segment");
                ReadSegment(r, LogicalType::Integer(), 4);
            },
            "a truncated dense sketch");
    }
    {
        BinaryWriter w = SketchBlock(4, 0);
        w.U8(1); // an empty sketch for a segment that has values
        w.U8(0);
        w.U8(0);
        ExpectSketchRejected(w, 4, "empty sketch, values present", "disagrees");
    }
    {
        BinaryWriter w = SketchBlock(4, 4);
        w.U8(1); // a sketch with a value for a segment of only NULLs
        w.U8(1);
        w.U8(0);
        w.U8(5);
        w.U8(0);
        w.U8(2);
        ExpectSketchRejected(w, 4, "values in an all-NULL segment", "disagrees");
    }
}

} // namespace cdb
