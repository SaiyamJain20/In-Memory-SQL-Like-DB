#include "storage/column_builder.h"

#include "storage_test_util.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

using test::BitIdentical;

// Reads a whole segment back through Scan(), vector by vector.
std::vector<Value> ReadSegment(const ColumnSegment& seg) {
    std::vector<Value> out;
    Vector v(seg.type(), kVectorSize);
    for (idx_t off = 0; off < seg.count(); off += kVectorSize) {
        const idx_t n = std::min(kVectorSize, seg.count() - off);
        seg.Scan(off, n, v);
        v.Verify(n);
        for (idx_t i = 0; i < n; i++)
            out.push_back(v.GetValue(i));
    }
    return out;
}

Vector MakeVector(LogicalType type, const std::vector<Value>& vals, idx_t from, idx_t n) {
    Vector v(type, kVectorSize);
    for (idx_t i = 0; i < n; i++)
        v.SetValue(i, vals[from + i]);
    return v;
}

// Appends `vals` to a builder in random-sized pieces.
void AppendRandomPieces(ColumnBuilder& b, const std::vector<Value>& vals, test::Rng& rng) {
    idx_t pos = 0;
    while (pos < vals.size()) {
        const idx_t n = std::min<idx_t>(1 + test::RandBelow(rng, kVectorSize), vals.size() - pos);
        Vector v = MakeVector(b.type(), vals, pos, n);
        b.Append(v, 0, n);
        pos += n;
    }
}

void ExpectSame(const std::vector<Value>& got, const std::vector<Value>& expect) {
    ASSERT_EQ(got.size(), expect.size());
    for (size_t i = 0; i < got.size(); i++) {
        ASSERT_TRUE(BitIdentical(got[i], expect[i]))
            << "row " << i << ": got " << got[i].ToString() << " expected " << expect[i].ToString();
    }
}

} // namespace

TEST(ColumnBuilder, RoundTripsEveryTypeThroughSnapshotAndSeal) {
    test::Rng rng(1);
    for (LogicalType type : test::AllTypes()) {
        for (idx_t total : {idx_t{0}, idx_t{1}, idx_t{2047}, idx_t{2048}, idx_t{2049}, idx_t{5000},
                            idx_t{8192}}) {
            std::vector<Value> vals;
            for (idx_t i = 0; i < total; i++)
                vals.push_back(test::RandomValue(rng, type, 0.15));
            ColumnBuilder b(type, 8192);
            AppendRandomPieces(b, vals, rng);
            ASSERT_EQ(b.count(), total);

            auto snap = b.Snapshot();
            EXPECT_EQ(snap->count(), total);
            ExpectSame(ReadSegment(*snap), vals);

            auto sealed = b.Seal();
            EXPECT_EQ(sealed->count(), total);
            ExpectSame(ReadSegment(*sealed), vals);
            EXPECT_EQ(b.count(), 0u); // the builder is empty after Seal
            // and an earlier snapshot is unaffected by sealing
            ExpectSame(ReadSegment(*snap), vals);
        }
    }
}

TEST(ColumnBuilder, SnapshotIsFrozenWhileTheBuilderKeepsGrowing) {
    test::Rng rng(2);
    for (LogicalType type : test::AllTypes()) {
        std::vector<Value> first, second;
        for (int i = 0; i < 1500; i++)
            first.push_back(test::RandomValue(rng, type, 0.2));
        for (int i = 0; i < 6000; i++)
            second.push_back(test::RandomValue(rng, type, 0.2));
        ColumnBuilder b(type, 8192);
        AppendRandomPieces(b, first, rng);
        auto snap = b.Snapshot();
        AppendRandomPieces(b, second, rng); // crosses several buffer growths

        ExpectSame(ReadSegment(*snap), first);
        std::vector<Value> all = first;
        all.insert(all.end(), second.begin(), second.end());
        ExpectSame(ReadSegment(*b.Snapshot()), all);
    }
}

TEST(ColumnBuilder, BuilderIsReusableAfterSeal) {
    ColumnBuilder b(LogicalType::Varchar(), 4096);
    std::vector<Value> a, c;
    for (int i = 0; i < 3000; i++)
        a.push_back(Value::Varchar(std::string(20 + i % 30, 'a' + i % 26)));
    for (int i = 0; i < 100; i++)
        c.push_back(Value::Varchar("second-" + std::to_string(i) + std::string(20, 'z')));
    test::Rng rng(3);
    AppendRandomPieces(b, a, rng);
    auto s1 = b.Seal();
    AppendRandomPieces(b, c, rng);
    auto s2 = b.Seal();
    ExpectSame(ReadSegment(*s1), a);
    ExpectSame(ReadSegment(*s2), c);
}

TEST(ColumnBuilder, FullBuilderSealsWithoutCopyingBuffers) {
    ColumnBuilder b(LogicalType::BigInt(), 4096);
    std::vector<Value> vals;
    for (int i = 0; i < 4096; i++)
        vals.push_back(Value::BigInt(i * 3));
    test::Rng rng(4);
    AppendRandomPieces(b, vals, rng);
    auto seg = b.Seal();
    ExpectSame(ReadSegment(*seg), vals);
    EXPECT_EQ(seg->stats().min->GetBigInt(), 0);
    EXPECT_EQ(seg->stats().max->GetBigInt(), 4095 * 3);
    // exactly the data (4096 * 8 bytes) plus padding - no growth slack retained
    EXPECT_LE(seg->MemoryUsage(), 4096u * 8 + Buffer::kPadding + 64);
}

TEST(ColumnBuilder, AppendsFromAnyVectorFormatAndOffset) {
    test::Rng rng(5);
    for (LogicalType type : test::AllTypes()) {
        std::vector<Value> vals;
        for (int i = 0; i < 300; i++)
            vals.push_back(test::RandomValue(rng, type, 0.3));
        Vector flat = MakeVector(type, vals, 0, 300);

        ColumnBuilder b(type, 4096);
        std::vector<Value> expect;

        // flat source with an offset
        b.Append(flat, 100, 50);
        expect.insert(expect.end(), vals.begin() + 100, vals.begin() + 150);

        // dictionary source
        Vector dict(type, kVectorSize);
        dict.Reference(flat);
        SelectionVector sel(5);
        const sel_t picks[5] = {299, 0, 0, 150, 7};
        for (int i = 0; i < 5; i++)
            sel.Set(i, picks[i]);
        dict.Slice(sel, 5);
        b.Append(dict, 1, 3); // dictionary rows 1..3 => picks 0, 0, 150
        expect.push_back(vals[0]);
        expect.push_back(vals[0]);
        expect.push_back(vals[150]);

        // constant source
        const Value c = test::RandomValue(rng, type, 0.3);
        Vector konst = Vector::MakeConstant(c, kVectorSize);
        b.Append(konst, 10, 20);
        for (int i = 0; i < 20; i++)
            expect.push_back(c);

        ExpectSame(ReadSegment(*b.Snapshot()), expect);
    }
}

TEST(ColumnBuilder, SegmentStatsMatchTheData) {
    test::Rng rng(6);
    for (LogicalType type : test::AllTypes()) {
        std::vector<Value> vals;
        for (int i = 0; i < 3333; i++)
            vals.push_back(test::RandomValue(rng, type, 0.25));
        ColumnBuilder b(type, 4096);
        AppendRandomPieces(b, vals, rng);
        auto seg = b.Snapshot();
        idx_t nulls = 0;
        for (const Value& v : vals)
            nulls += v.IsNull();
        EXPECT_EQ(seg->stats().count, 3333u);
        EXPECT_EQ(seg->stats().null_count, nulls);
        if (seg->stats().min.has_value()) {
            for (const Value& v : vals) {
                if (v.IsNull())
                    continue;
                ASSERT_GE(Value::Compare(v, *seg->stats().min), 0);
                ASSERT_LE(Value::Compare(v, *seg->stats().max), 0);
            }
        }
    }
}

// ------------------------------------------------------------------ Scan() views

TEST(ColumnSegment, ScanIsZeroCopyAndStable) {
    // Zero-copy scans are a property of RAW segments; an encoded segment decodes into the output
    // (these values are 0..4095 and would otherwise be bit-packed), so compression is off here.
    const test::ScopedCompression raw_layout(false);
    ColumnBuilder b(LogicalType::Integer(), 4096);
    std::vector<Value> vals;
    for (int i = 0; i < 4096; i++)
        vals.push_back(Value::Integer(i));
    test::Rng rng(7);
    AppendRandomPieces(b, vals, rng);
    auto seg = b.Seal();

    Vector a(LogicalType::Integer(), kVectorSize), c(LogicalType::Integer(), kVectorSize);
    seg->Scan(2048, 2048, a);
    seg->Scan(2048, 2048, c);
    EXPECT_EQ(a.format(), VectorFormat::Flat);
    const Vector& ca = a;
    const Vector& cc = c;
    EXPECT_EQ(ca.FlatBytes(), cc.FlatBytes()); // both point into the segment: no copy
    EXPECT_EQ(a.GetValue(0).GetInteger(), 2048);
    EXPECT_EQ(a.GetValue(2047).GetInteger(), 4095);
    EXPECT_EQ(seg->scan_calls(), 2u);
}

TEST(ColumnSegment, ResetOfAScanVectorNeverTouchesTheSegment) {
    ColumnBuilder b(LogicalType::BigInt(), 2048);
    std::vector<Value> vals;
    for (int i = 0; i < 2048; i++)
        vals.push_back(Value::BigInt(i + 1000));
    test::Rng rng(8);
    AppendRandomPieces(b, vals, rng);
    auto seg = b.Seal();

    Vector v(LogicalType::BigInt(), kVectorSize);
    seg->Scan(0, 2048, v);
    v.Reset(); // must detach from the segment's read-only buffer
    for (int i = 0; i < 2048; i++)
        v.SetValue(i, Value::BigInt(-1));
    ExpectSame(ReadSegment(*seg), vals);
}

TEST(ColumnSegment, ScanVectorsKeepTheSegmentMemoryAlive) {
    Vector v(LogicalType::Varchar(), kVectorSize);
    std::vector<Value> vals;
    {
        ColumnBuilder b(LogicalType::Varchar(), 2048);
        for (int i = 0; i < 100; i++) {
            vals.push_back(Value::Varchar(std::string(30 + i, static_cast<char>('A' + i % 26))));
        }
        test::Rng rng(9);
        AppendRandomPieces(b, vals, rng);
        auto seg = b.Seal();
        seg->Scan(0, 100, v);
        // `seg` and `b` are destroyed here; the vector must still be readable (ASan checks this)
    }
    for (int i = 0; i < 100; i++)
        ASSERT_EQ(v.GetValue(i).GetVarchar(), vals[i].GetVarchar());
}

TEST(ColumnSegment, ValidityViewsAreCorrectPerVector) {
    test::Rng rng(10);
    std::vector<Value> vals;
    for (int i = 0; i < 5000; i++)
        vals.push_back(test::RandomValue(rng, LogicalType::Integer(), 0.3));
    ColumnBuilder b(LogicalType::Integer(), 8192);
    AppendRandomPieces(b, vals, rng);
    auto seg = b.Seal();
    // Read vectors in reverse order, to catch any offset bookkeeping error.
    Vector v(LogicalType::Integer(), kVectorSize);
    for (int vi = 2; vi >= 0; vi--) {
        const idx_t off = static_cast<idx_t>(vi) * kVectorSize;
        const idx_t n = std::min<idx_t>(kVectorSize, 5000 - off);
        seg->Scan(off, n, v);
        for (idx_t i = 0; i < n; i++)
            ASSERT_TRUE(BitIdentical(v.GetValue(i), vals[off + i]));
    }
}

TEST(ColumnSegment, FullyValidSegmentsYieldAllValidVectors) {
    ColumnBuilder b(LogicalType::Integer(), 2048);
    Vector src(LogicalType::Integer(), kVectorSize);
    for (int i = 0; i < 100; i++)
        src.SetValue(i, Value::Integer(i));
    b.Append(src, 0, 100);
    auto seg = b.Seal();
    Vector v(LogicalType::Integer(), kVectorSize);
    seg->Scan(0, 100, v);
    EXPECT_TRUE(v.Validity().AllValid());
}

TEST(ColumnSegmentDeathTest, ScanContractViolationsAbort) {
    ColumnBuilder b(LogicalType::Integer(), 4096);
    Vector src(LogicalType::Integer(), kVectorSize);
    b.Append(src, 0, 2048);
    b.Append(src, 0, 100);
    auto seg = b.Seal();
    Vector v(LogicalType::Integer(), kVectorSize);
    EXPECT_DEATH(seg->Scan(100, 10, v), "CDB_CHECK");    // offset not a vector boundary
    EXPECT_DEATH(seg->Scan(2048, 2048, v), "CDB_CHECK"); // runs past the end
    EXPECT_DEATH(seg->Scan(0, 2049, v), "CDB_CHECK");    // more than a vector
    Vector small(LogicalType::Integer(), 64);
    EXPECT_DEATH(seg->Scan(0, 10, small), "CDB_CHECK"); // output vector too small
    Vector wrong(LogicalType::BigInt(), kVectorSize);
    EXPECT_DEATH(seg->Scan(0, 10, wrong), "CDB_CHECK"); // wrong type
}

TEST(ColumnSegmentDeathTest, ScanVectorsAreReadOnly) {
    ColumnBuilder b(LogicalType::Integer(), 2048);
    Vector src(LogicalType::Integer(), kVectorSize);
    b.Append(src, 0, 10);
    auto seg = b.Seal();
    Vector v(LogicalType::Integer(), kVectorSize);
    seg->Scan(0, 10, v);
    EXPECT_DEATH(v.SetValue(0, Value::Integer(1)), "CDB_CHECK"); // always-on check
#if defined(CDB_ENABLE_ASSERTS)
    EXPECT_DEATH(v.FlatData<int32_t>(), "CDB_ASSERT");
#endif
}

TEST(ColumnBuilderDeathTest, ContractViolationsAbort) {
    EXPECT_DEATH(ColumnBuilder(LogicalType::Integer(), 100), "CDB_CHECK"); // not a multiple
    ColumnBuilder b(LogicalType::Integer(), 2048);
    Vector wrong(LogicalType::BigInt(), kVectorSize);
    EXPECT_DEATH(b.Append(wrong, 0, 1), "CDB_CHECK");
    Vector ok(LogicalType::Integer(), kVectorSize);
    b.Append(ok, 0, 2048);
    EXPECT_DEATH(b.Append(ok, 0, 1), "CDB_CHECK"); // beyond max_rows
}

} // namespace cdb
