#include "vector/vector.h"
#include <cstring>

#include "test_util.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cdb {

namespace {

using test::AllTypes;
using test::BitIdentical;

Vector MakeFlat(LogicalType type, const std::vector<Value>& vals, idx_t capacity = kVectorSize) {
    Vector v(type, capacity);
    for (idx_t i = 0; i < vals.size(); i++) {
        v.SetValue(i, vals[i]);
    }
    return v;
}

std::vector<Value> RandomValues(test::Rng& rng, LogicalType type, idx_t n, double nulls = 0.2) {
    std::vector<Value> vals;
    for (idx_t i = 0; i < n; i++)
        vals.push_back(test::RandomValue(rng, type, nulls));
    return vals;
}

void ExpectEquals(const Vector& v, const std::vector<Value>& expect) {
    for (idx_t i = 0; i < expect.size(); i++) {
        const Value got = v.GetValue(i);
        ASSERT_TRUE(BitIdentical(got, expect[i]))
            << "row " << i << ": got " << got.ToString() << " expected " << expect[i].ToString();
    }
}

SelectionVector MakeSel(const std::vector<sel_t>& idx) {
    SelectionVector s(idx.size());
    for (idx_t i = 0; i < idx.size(); i++)
        s.Set(i, idx[i]);
    return s;
}

} // namespace

// ---------------------------------------------------------------- construction / flat access

TEST(Vector, NewFlatVectorIsAllValidAndZeroed) {
    for (LogicalType t : AllTypes()) {
        for (idx_t cap : {idx_t{1}, idx_t{7}, idx_t{64}, idx_t{65}, kVectorSize}) {
            Vector v(t, cap);
            EXPECT_EQ(v.format(), VectorFormat::Flat);
            EXPECT_EQ(v.capacity(), cap);
            EXPECT_EQ(v.type(), t);
            EXPECT_TRUE(v.Validity().AllValid());
            for (idx_t i = 0; i < cap; i++) {
                Value x = v.GetValue(i);
                ASSERT_FALSE(x.IsNull()) << t.ToString();
            }
            v.Verify(cap);
        }
    }
}

TEST(Vector, SetGetRoundTripAllTypesWithNulls) {
    test::Rng rng(1);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, kVectorSize, 0.25);
        Vector v = MakeFlat(t, vals);
        ExpectEquals(v, vals);
        v.Verify(kVectorSize);
    }
}

TEST(Vector, ExtremeValuesRoundTripBitExactly) {
    Vector d(LogicalType::Double(), 8);
    const double specials[] = {0.0,
                               -0.0,
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::denorm_min(),
                               std::numeric_limits<double>::max(),
                               std::numeric_limits<double>::lowest()};
    for (int i = 0; i < 8; i++)
        d.SetValue(i, Value::Double(specials[i]));
    for (int i = 0; i < 8; i++) {
        EXPECT_TRUE(BitIdentical(d.GetValue(i), Value::Double(specials[i]))) << i;
    }
    Vector ints(LogicalType::BigInt(), 2);
    ints.SetValue(0, Value::BigInt(std::numeric_limits<int64_t>::min()));
    ints.SetValue(1, Value::BigInt(std::numeric_limits<int64_t>::max()));
    EXPECT_EQ(ints.GetValue(0).GetBigInt(), std::numeric_limits<int64_t>::min());
    EXPECT_EQ(ints.GetValue(1).GetBigInt(), std::numeric_limits<int64_t>::max());
}

TEST(Vector, ValidityMaskIsAllocatedOnlyWhenANullAppears) {
    Vector v(LogicalType::Integer(), 100);
    for (int i = 0; i < 100; i++)
        v.SetValue(i, Value::Integer(i));
    EXPECT_TRUE(v.Validity().AllValid());
    v.SetValue(50, Value::Null(LogicalType::Integer()));
    EXPECT_FALSE(v.Validity().AllValid());
    EXPECT_TRUE(v.GetValue(50).IsNull());
    EXPECT_FALSE(v.GetValue(49).IsNull());
    // writing a value over a NULL makes the row valid again
    v.SetValue(50, Value::Integer(-1));
    EXPECT_EQ(v.GetValue(50).GetInteger(), -1);
}

TEST(Vector, VarcharLengthsAroundInlineBoundary) {
    Vector v(LogicalType::Varchar(), 64);
    for (uint32_t len = 0; len < 64; len++) {
        v.SetValue(len, Value::Varchar(std::string(len, static_cast<char>('a' + len % 26))));
    }
    for (uint32_t len = 0; len < 64; len++) {
        EXPECT_EQ(v.GetValue(len).GetVarchar(),
                  std::string(len, static_cast<char>('a' + len % 26)));
    }
    v.Verify(64);
}

TEST(Vector, OverwritingAStringKeepsOtherRowsIntact) {
    Vector v(LogicalType::Varchar(), 4);
    v.SetValue(0, Value::Varchar(std::string(40, 'a')));
    v.SetValue(1, Value::Varchar(std::string(50, 'b')));
    v.SetValue(0, Value::Varchar("short"));
    v.SetValue(1, Value::Varchar(std::string(60, 'c')));
    EXPECT_EQ(v.GetValue(0).GetVarchar(), "short");
    EXPECT_EQ(v.GetValue(1).GetVarchar(), std::string(60, 'c'));
}

TEST(Vector, FlatDataExposesTypedArray) {
    Vector v(LogicalType::Integer(), 10);
    int32_t* p = v.FlatData<int32_t>();
    for (int i = 0; i < 10; i++)
        p[i] = i * i;
    EXPECT_EQ(v.GetValue(7).GetInteger(), 49);
    const Vector& cv = v;
    EXPECT_EQ(cv.FlatData<int32_t>()[9], 81);
}

TEST(Vector, DateAndIntegerShareStorageButNotType) {
    Vector v(LogicalType::Date(), 3);
    v.SetValue(0, Value::Date(Date::FromYMD(1998, 12, 1)));
    EXPECT_EQ(v.FlatData<int32_t>()[0], 10561);
    EXPECT_EQ(v.GetValue(0).type(), LogicalType::Date());
    EXPECT_EQ(v.GetValue(0).ToString(), "1998-12-01");
}

// ---------------------------------------------------------------- constant vectors

TEST(VectorConstant, EveryRowReadsTheSameValue) {
    test::Rng rng(2);
    for (LogicalType t : AllTypes()) {
        for (int trial = 0; trial < 20; trial++) {
            Value c = test::RandomValue(rng, t, 0.3);
            Vector v = Vector::MakeConstant(c);
            EXPECT_EQ(v.format(), VectorFormat::Constant);
            for (idx_t row : {idx_t{0}, idx_t{1}, idx_t{777}, kVectorSize - 1}) {
                ASSERT_TRUE(BitIdentical(v.GetValue(row), c)) << t.ToString() << " row " << row;
            }
            v.Verify(kVectorSize);
        }
    }
}

TEST(VectorConstant, LongStringConstantSurvivesFlatten) {
    const std::string s(100, 'k');
    Vector v = Vector::MakeConstant(Value::Varchar(s), 50);
    v.Flatten(50);
    EXPECT_EQ(v.format(), VectorFormat::Flat);
    for (idx_t i = 0; i < 50; i++)
        ASSERT_EQ(v.GetValue(i).GetVarchar(), s);
    v.Verify(50);
}

TEST(VectorConstant, UnifiedFormatUsesZeroSelection) {
    Vector v = Vector::MakeConstant(Value::Integer(9));
    UnifiedFormat u;
    v.ToUnified(u);
    EXPECT_EQ(u.sel, SelectionVector::Zeros().data());
    for (idx_t i = 0; i < kVectorSize; i += 100) {
        EXPECT_TRUE(u.IsValid(i));
        EXPECT_EQ(u.Data<int32_t>()[u.sel[i]], 9);
    }
}

TEST(VectorConstant, FlattenReplicatesExactlyCountRows) {
    for (LogicalType t : AllTypes()) {
        Value c = t.id() == TypeId::Varchar
                      ? Value::Varchar("a constant string value")
                      : (t.id() == TypeId::Boolean ? Value::Boolean(true) : Value::Null(t));
        if (t.id() == TypeId::Integer)
            c = Value::Integer(5);
        for (idx_t count : {idx_t{0}, idx_t{1}, idx_t{63}, idx_t{64}, idx_t{65}, kVectorSize}) {
            Vector v = Vector::MakeConstant(c);
            v.Flatten(count);
            EXPECT_EQ(v.format(), VectorFormat::Flat);
            for (idx_t i = 0; i < count; i++)
                ASSERT_TRUE(BitIdentical(v.GetValue(i), c));
            v.Verify(count);
        }
    }
}

TEST(VectorConstant, NullConstantFlattensToAllNulls) {
    Vector v = Vector::MakeConstant(Value::Null(LogicalType::Double()), 100);
    EXPECT_TRUE(v.GetValue(0).IsNull());
    EXPECT_TRUE(v.GetValue(99).IsNull());
    v.Flatten(100);
    for (idx_t i = 0; i < 100; i++)
        ASSERT_TRUE(v.GetValue(i).IsNull());
}

TEST(VectorConstant, SliceIsANoOp) {
    Vector v = Vector::MakeConstant(Value::BigInt(5), 100);
    v.Slice(MakeSel({3, 1, 4}), 3);
    EXPECT_EQ(v.format(), VectorFormat::Constant);
    EXPECT_EQ(v.GetValue(2).GetBigInt(), 5);
}

TEST(VectorConstant, ResetReturnsToWritableFlat) {
    Vector v(LogicalType::Integer(), 8);
    v.SetConstant(Value::Integer(3));
    EXPECT_EQ(v.format(), VectorFormat::Constant);
    v.Reset();
    EXPECT_EQ(v.format(), VectorFormat::Flat);
    v.SetValue(7, Value::Integer(11));
    EXPECT_EQ(v.GetValue(7).GetInteger(), 11);
    v.Verify(8);
}

TEST(VectorConstant, SetConstantOverExistingFlatVector) {
    Vector v(LogicalType::Varchar(), 8);
    v.SetValue(0, Value::Varchar(std::string(30, 'x')));
    v.SetValue(1, Value::Null(LogicalType::Varchar()));
    v.SetConstant(Value::Varchar("zzz"));
    for (idx_t i = 0; i < 8; i++)
        ASSERT_EQ(v.GetValue(i).GetVarchar(), "zzz");
    v.SetConstant(Value::Null(LogicalType::Varchar()));
    for (idx_t i = 0; i < 8; i++)
        ASSERT_TRUE(v.GetValue(i).IsNull());
}

// ---------------------------------------------------------------- dictionary vectors / Slice

TEST(VectorDictionary, SliceFlatSelectsReordersAndRepeats) {
    test::Rng rng(3);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 100);
        Vector v = MakeFlat(t, vals, 128);
        const std::vector<sel_t> idx = {99, 0, 0, 50, 3, 3, 3, 98, 1};
        v.Slice(MakeSel(idx), idx.size());
        EXPECT_EQ(v.format(), VectorFormat::Dictionary);
        EXPECT_EQ(v.DictionaryChild().format(), VectorFormat::Flat);
        std::vector<Value> expect;
        for (sel_t i : idx)
            expect.push_back(vals[i]);
        ExpectEquals(v, expect);
        v.Verify(idx.size());
    }
}

TEST(VectorDictionary, SliceDoesNotCopyData) {
    Vector v(LogicalType::Integer(), 100);
    for (int i = 0; i < 100; i++)
        v.SetValue(i, Value::Integer(i));
    const int32_t* before = v.FlatData<int32_t>();
    v.Slice(MakeSel({5, 6, 7}), 3);
    UnifiedFormat u;
    v.ToUnified(u);
    EXPECT_EQ(u.Data<int32_t>(), before); // same buffer, reached through the selection
}

TEST(VectorDictionary, EmptyAndSingleRowAndFullSelections) {
    Vector v(LogicalType::BigInt(), 10);
    for (int i = 0; i < 10; i++)
        v.SetValue(i, Value::BigInt(i * 7));
    Vector a(LogicalType::BigInt(), 10);
    a.Reference(v);
    a.Slice(MakeSel({}), 0);
    EXPECT_EQ(a.format(), VectorFormat::Dictionary);
    a.Verify(0);

    Vector b(LogicalType::BigInt(), 10);
    b.Reference(v);
    b.Slice(MakeSel({9}), 1);
    EXPECT_EQ(b.GetValue(0).GetBigInt(), 63);

    Vector c(LogicalType::BigInt(), 10);
    c.Reference(v);
    std::vector<sel_t> all(10);
    for (sel_t i = 0; i < 10; i++)
        all[i] = i;
    c.Slice(MakeSel(all), 10);
    for (int i = 0; i < 10; i++)
        EXPECT_EQ(c.GetValue(i).GetBigInt(), i * 7);
}

TEST(VectorDictionary, SliceOfSliceComposesAndKeepsDepthOne) {
    test::Rng rng(4);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 50);
        Vector v = MakeFlat(t, vals, 64);
        v.Slice(MakeSel({10, 20, 20, 49, 0, 33}), 6);
        const Vector* child = &v.DictionaryChild();
        v.Slice(MakeSel({5, 5, 1, 0}), 4);      // -> original rows 33, 33, 20, 10
        EXPECT_EQ(&v.DictionaryChild(), child); // composed, not nested
        EXPECT_EQ(v.DictionaryChild().format(), VectorFormat::Flat);
        ExpectEquals(v, {vals[33], vals[33], vals[20], vals[10]});
        v.Verify(4);
    }
}

TEST(VectorDictionary, FlattenMatchesDictionaryView) {
    test::Rng rng(5);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 200);
        Vector v = MakeFlat(t, vals, 256);
        std::vector<sel_t> idx;
        std::vector<Value> expect;
        for (int i = 0; i < 150; i++) {
            idx.push_back(static_cast<sel_t>(test::RandBelow(rng, 200)));
            expect.push_back(vals[idx.back()]);
        }
        v.Slice(MakeSel(idx), idx.size());
        v.Flatten(idx.size());
        EXPECT_EQ(v.format(), VectorFormat::Flat);
        ExpectEquals(v, expect);
        v.Verify(idx.size());
    }
}

TEST(VectorDictionary, FlattenedStringsKeepTheirHeapAlive) {
    // After Flatten the dictionary child is gone; long strings must still be readable, which
    // requires the flat vector to have taken over (shared) the child's heap. ASan checks this.
    Vector v(LogicalType::Varchar(), 16);
    for (int i = 0; i < 16; i++)
        v.SetValue(i, Value::Varchar(std::string(30 + i, 'a' + i)));
    v.Slice(MakeSel({15, 0, 7}), 3);
    v.Flatten(3);
    EXPECT_EQ(v.GetValue(0).GetVarchar(), std::string(45, 'p'));
    EXPECT_EQ(v.GetValue(1).GetVarchar(), std::string(30, 'a'));
    EXPECT_EQ(v.GetValue(2).GetVarchar(), std::string(37, 'h'));
}

TEST(VectorDictionary, NullsPropagateThroughSelection) {
    Vector v(LogicalType::Integer(), 8);
    for (int i = 0; i < 8; i++) {
        v.SetValue(i, i % 2 == 0 ? Value::Null(LogicalType::Integer()) : Value::Integer(i));
    }
    v.Slice(MakeSel({1, 0, 3, 2}), 4);
    EXPECT_FALSE(v.GetValue(0).IsNull());
    EXPECT_TRUE(v.GetValue(1).IsNull());
    EXPECT_FALSE(v.GetValue(2).IsNull());
    EXPECT_TRUE(v.GetValue(3).IsNull());
}

TEST(VectorDictionary, UnifiedFormatReadsThroughSelection) {
    Vector v(LogicalType::Integer(), 10);
    for (int i = 0; i < 10; i++)
        v.SetValue(i, Value::Integer(i * 100));
    v.SetValue(4, Value::Null(LogicalType::Integer()));
    v.Slice(MakeSel({9, 4, 1}), 3);
    UnifiedFormat u;
    v.ToUnified(u);
    EXPECT_EQ(u.Data<int32_t>()[u.sel[0]], 900);
    EXPECT_FALSE(u.IsValid(1));
    EXPECT_EQ(u.Data<int32_t>()[u.sel[2]], 100);
}

TEST(VectorDictionary, ResetReturnsToWritableFlat) {
    Vector v(LogicalType::Integer(), 8);
    for (int i = 0; i < 8; i++)
        v.SetValue(i, Value::Integer(i));
    v.SetValue(2, Value::Null(LogicalType::Integer()));
    v.Slice(MakeSel({2, 3}), 2);
    v.Reset();
    EXPECT_EQ(v.format(), VectorFormat::Flat);
    EXPECT_TRUE(v.Validity().AllValid());
    for (int i = 0; i < 8; i++)
        v.SetValue(i, Value::Integer(100 + i));
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(v.GetValue(i).GetInteger(), 100 + i);
    v.Verify(8);
}

// ---------------------------------------------------------------- Reference / Reset sharing

TEST(VectorReference, SharesValuesAcrossAllFormats) {
    test::Rng rng(6);
    auto vals = RandomValues(rng, LogicalType::Varchar(), 30);
    Vector flat = MakeFlat(LogicalType::Varchar(), vals, 32);

    Vector r1(LogicalType::Varchar(), 32);
    r1.Reference(flat);
    ExpectEquals(r1, vals);

    Vector dict(LogicalType::Varchar(), 32);
    dict.Reference(flat);
    dict.Slice(MakeSel({4, 2}), 2);
    Vector r2(LogicalType::Varchar(), 32);
    r2.Reference(dict);
    EXPECT_EQ(r2.format(), VectorFormat::Dictionary);
    ExpectEquals(r2, {vals[4], vals[2]});

    Vector c = Vector::MakeConstant(Value::Varchar("const"), 32);
    Vector r3(LogicalType::Varchar(), 32);
    r3.Reference(c);
    EXPECT_EQ(r3.GetValue(31).GetVarchar(), "const");
}

TEST(VectorReference, ResetOfOneSideNeverCorruptsTheOther) {
    // The producer reuses its vector (Reset + refill) while a consumer still holds a reference
    // to the previous contents: the consumer must keep seeing the old data, including strings.
    Vector producer(LogicalType::Varchar(), 16);
    std::vector<Value> first;
    for (int i = 0; i < 16; i++) {
        first.push_back(Value::Varchar(std::string(20 + i, static_cast<char>('A' + i))));
        producer.SetValue(i, first.back());
    }
    producer.SetValue(3, Value::Null(LogicalType::Varchar()));
    first[3] = Value::Null(LogicalType::Varchar());

    Vector consumer(LogicalType::Varchar(), 16);
    consumer.Reference(producer);

    producer.Reset();
    for (int i = 0; i < 16; i++) {
        producer.SetValue(i, Value::Varchar(std::string(25, static_cast<char>('a' + i))));
    }
    ExpectEquals(consumer, first);
    EXPECT_EQ(producer.GetValue(3).GetVarchar(), std::string(25, 'd'));
}

TEST(VectorReference, ResetWithoutSharingReusesTheBuffer) {
    Vector v(LogicalType::Integer(), 64);
    const int32_t* before = v.FlatData<int32_t>();
    v.Reset();
    EXPECT_EQ(v.FlatData<int32_t>(), before); // no needless reallocation in the steady state
}

TEST(VectorReference, SlicingTheCopyLeavesTheOriginalUntouched) {
    Vector a(LogicalType::Integer(), 10);
    for (int i = 0; i < 10; i++)
        a.SetValue(i, Value::Integer(i));
    Vector b(LogicalType::Integer(), 10);
    b.Reference(a);
    b.Slice(MakeSel({9, 8}), 2);
    EXPECT_EQ(a.format(), VectorFormat::Flat);
    for (int i = 0; i < 10; i++)
        EXPECT_EQ(a.GetValue(i).GetInteger(), i);
    EXPECT_EQ(b.GetValue(0).GetInteger(), 9);
}

TEST(VectorReference, ProducerHeapIsClearedOnResetWhenUnshared) {
    Vector v(LogicalType::Varchar(), 4);
    v.SetValue(0, Value::Varchar(std::string(500, 'x')));
    EXPECT_GT(v.Heap().BytesUsed(), 0u);
    v.Reset();
    EXPECT_EQ(v.Heap().BytesUsed(), 0u);
}

// ---------------------------------------------------------------- VectorOps::Copy

TEST(VectorCopy, FlatToFlatWithoutSelection) {
    test::Rng rng(7);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 300);
        Vector src = MakeFlat(t, vals);
        Vector dst(t);
        VectorOps::Copy(src, dst, nullptr, 300);
        ExpectEquals(dst, vals);
        dst.Verify(300);
    }
}

TEST(VectorCopy, HonoursSelectionAndDestinationOffset) {
    test::Rng rng(8);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 100);
        Vector src = MakeFlat(t, vals);
        const std::vector<sel_t> idx = {99, 3, 3, 50, 0};
        SelectionVector sel = MakeSel(idx);

        auto sentinels = RandomValues(rng, t, 40);
        Vector dst = MakeFlat(t, sentinels);
        VectorOps::Copy(src, dst, &sel, idx.size(), 10);

        for (idx_t i = 0; i < 40; i++) {
            const Value expect =
                (i >= 10 && i < 15) ? vals[idx[i - 10]] : sentinels[i]; // others untouched
            ASSERT_TRUE(BitIdentical(dst.GetValue(i), expect)) << t.ToString() << " row " << i;
        }
        dst.Verify(40);
    }
}

TEST(VectorCopy, OverwritesPreviousNullsAndValidity) {
    Vector dst(LogicalType::Integer(), 4);
    for (int i = 0; i < 4; i++)
        dst.SetValue(i, Value::Null(LogicalType::Integer()));
    Vector src(LogicalType::Integer(), 4);
    src.SetValue(0, Value::Integer(10));
    src.SetValue(1, Value::Null(LogicalType::Integer()));
    src.SetValue(2, Value::Integer(30));
    src.SetValue(3, Value::Integer(40));
    VectorOps::Copy(src, dst, nullptr, 4);
    EXPECT_EQ(dst.GetValue(0).GetInteger(), 10);
    EXPECT_TRUE(dst.GetValue(1).IsNull());
    EXPECT_EQ(dst.GetValue(2).GetInteger(), 30);
    EXPECT_EQ(dst.GetValue(3).GetInteger(), 40);
}

TEST(VectorCopy, NullFreeSourceClearsNullsPreviouslyInTheDestination) {
    // The source has no NULLs, so Copy takes the "mark the range valid" path; every NULL the
    // destination held inside the copied range must disappear, and those outside must stay.
    test::Rng rng(10);
    for (LogicalType t : AllTypes()) {
        for (auto [offset, count] : {std::pair<idx_t, idx_t>{0, 200},
                                     {3, 130},
                                     {63, 2},
                                     {64, 64},
                                     {100, 1},
                                     {1, 198},
                                     {0, 0}}) {
            Vector dst(t, 256);
            std::vector<Value> before;
            for (idx_t i = 0; i < 256; i++) {
                before.push_back(i % 3 == 0 ? Value::Null(t) : test::RandomValue(rng, t, 0.0));
                dst.SetValue(i, before.back());
            }
            auto vals = RandomValues(rng, t, count, /*nulls=*/0.0);
            Vector src = MakeFlat(t, vals, 256);
            ASSERT_TRUE(src.Validity().AllValid());
            VectorOps::Copy(src, dst, nullptr, count, offset);
            for (idx_t i = 0; i < 256; i++) {
                const bool inside = i >= offset && i < offset + count;
                ASSERT_TRUE(BitIdentical(dst.GetValue(i), inside ? vals[i - offset] : before[i]))
                    << t.ToString() << " offset=" << offset << " count=" << count << " row " << i;
            }
            dst.Verify(256);
        }
    }
}

TEST(VectorCopy, FromDictionaryAndConstantSources) {
    test::Rng rng(9);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 64);
        Vector dict = MakeFlat(t, vals, 64);
        dict.Slice(MakeSel({63, 0, 31, 31}), 4);
        Vector out(t, 64);
        VectorOps::Copy(dict, out, nullptr, 4);
        ExpectEquals(out, {vals[63], vals[0], vals[31], vals[31]});

        const Value c = test::RandomValue(rng, t, 0.3);
        Vector konst = Vector::MakeConstant(c, 64);
        Vector out2(t, 64);
        VectorOps::Copy(konst, out2, nullptr, 64);
        for (idx_t i = 0; i < 64; i++)
            ASSERT_TRUE(BitIdentical(out2.GetValue(i), c));

        // selection applied on top of a dictionary source indexes its *logical* rows
        SelectionVector sel = MakeSel({3, 1});
        Vector out3(t, 64);
        VectorOps::Copy(dict, out3, &sel, 2);
        ExpectEquals(out3, {vals[31], vals[0]});
    }
}

TEST(VectorCopy, DestinationStringsAreIndependentOfTheSource) {
    Vector src(LogicalType::Varchar(), 8);
    std::vector<Value> vals;
    for (int i = 0; i < 8; i++) {
        vals.push_back(Value::Varchar(std::string(40 + i, static_cast<char>('m' + i))));
        src.SetValue(i, vals.back());
    }
    Vector dst(LogicalType::Varchar(), 8);
    VectorOps::Copy(src, dst, nullptr, 8);

    // destroy / recycle the source completely
    src.Reset();
    for (int i = 0; i < 8; i++)
        src.SetValue(i, Value::Varchar(std::string(45, '#')));
    ExpectEquals(dst, vals);
}

TEST(VectorCopy, ZeroCountIsANoOp) {
    Vector src(LogicalType::Integer(), 4), dst(LogicalType::Integer(), 4);
    dst.SetValue(0, Value::Integer(5));
    VectorOps::Copy(src, dst, nullptr, 0);
    EXPECT_EQ(dst.GetValue(0).GetInteger(), 5);
}

// ---------------------------------------------------------------- Verify / contracts

TEST(VectorDeathTest, VerifyCatchesBrokenInlineStringPadding) {
    Vector v(LogicalType::Varchar(), 4);
    v.SetValue(0, Value::Varchar("abc"));
    reinterpret_cast<uint8_t*>(v.FlatData<string_t>())[15] = 1; // garbage in the padding
    EXPECT_DEATH(v.Verify(4), "CDB_CHECK");
}

TEST(VectorDeathTest, VerifyCatchesOutOfRangeDictionaryIndex) {
    Vector v(LogicalType::Integer(), 8);
    v.Slice(MakeSel({1, 2}), 2);
    const_cast<sel_t*>(v.DictionarySel().data())[1] = 5000; // deliberate corruption
    EXPECT_DEATH(v.Verify(2), "CDB_CHECK");
}

TEST(VectorDeathTest, ContractViolationsAbort) {
    EXPECT_DEATH(Vector(LogicalType::Integer(), 0), "CDB_CHECK");
    EXPECT_DEATH(Vector(LogicalType::Integer(), kVectorSize + 1), "CDB_CHECK");

    Vector v(LogicalType::Integer(), 4);
    EXPECT_DEATH(v.SetValue(0, Value::BigInt(1)), "CDB_CHECK");  // wrong type
    EXPECT_DEATH(v.SetValue(4, Value::Integer(1)), "CDB_CHECK"); // out of range
    EXPECT_DEATH(v.GetValue(4), "CDB_CHECK");
    EXPECT_DEATH(v.Flatten(5), "CDB_CHECK");
    EXPECT_DEATH(v.Slice(MakeSel({0}), 5), "CDB_CHECK");

    v.Slice(MakeSel({0}), 1);
    EXPECT_DEATH(v.SetValue(0, Value::Integer(1)), "CDB_CHECK"); // not flat any more

    Vector a(LogicalType::Integer(), 4), b(LogicalType::BigInt(), 4);
    EXPECT_DEATH(VectorOps::Copy(a, b, nullptr, 1), "CDB_CHECK");
    Vector c(LogicalType::Integer(), 4);
    c.Slice(MakeSel({0}), 1);
    EXPECT_DEATH(VectorOps::Copy(a, c, nullptr, 1), "CDB_CHECK"); // dst must be flat
    EXPECT_DEATH(VectorOps::Copy(a, a, nullptr, 5), "CDB_CHECK"); // overflows dst
}

// ---------------------------------------------------------------- ReferenceFlat / CopyRows

TEST(VectorReferenceFlat, ViewsExternalBuffersZeroCopy) {
    Vector owner(LogicalType::BigInt(), kVectorSize);
    for (idx_t i = 0; i < kVectorSize; i++) {
        owner.SetValue(i, i % 5 == 0 ? Value::Null(LogicalType::BigInt())
                                     : Value::BigInt(static_cast<int64_t>(i) * 10));
    }
    auto data = Buffer::View(Buffer::Allocate(1), 0, 0); // unrelated, to prove nothing is shared
    (void)data;
    auto parent = Buffer::Allocate(kVectorSize * 8);
    std::memcpy(parent->data(), owner.FlatData<int64_t>(), kVectorSize * 8);
    auto view = Buffer::View(parent, 0, kVectorSize * 8);

    Vector v(LogicalType::BigInt(), kVectorSize);
    v.ReferenceFlat(view, owner.Validity(), nullptr);
    EXPECT_EQ(v.format(), VectorFormat::Flat);
    const Vector& cv = v;
    EXPECT_EQ(cv.FlatBytes(), parent->data());
    for (idx_t i = 0; i < kVectorSize; i++)
        ASSERT_TRUE(BitIdentical(v.GetValue(i), owner.GetValue(i)));
    v.Verify(kVectorSize);
}

TEST(VectorReferenceFlat, ResetDetachesWithoutTouchingTheViewedBuffer) {
    auto parent = Buffer::Allocate(kVectorSize * 4);
    for (idx_t i = 0; i < kVectorSize; i++)
        parent->As<int32_t>()[i] = static_cast<int32_t>(i);
    Vector v(LogicalType::Integer(), kVectorSize);
    v.ReferenceFlat(Buffer::View(parent, 0, kVectorSize * 4), ValidityMask(kVectorSize), nullptr);

    v.Reset();
    for (idx_t i = 0; i < kVectorSize; i++)
        v.SetValue(i, Value::Integer(-7));
    for (idx_t i = 0; i < kVectorSize; i++)
        ASSERT_EQ(parent->As<int32_t>()[i], static_cast<int32_t>(i));
    EXPECT_EQ(v.GetValue(5).GetInteger(), -7);
}

TEST(VectorReferenceFlat, ReplacesWhateverFormatTheVectorHad) {
    Vector v(LogicalType::Integer(), 8);
    v.SetConstant(Value::Integer(3));
    auto parent = Buffer::Allocate(8 * 4);
    parent->As<int32_t>()[2] = 42;
    v.ReferenceFlat(Buffer::View(parent, 0, 32), ValidityMask(8), nullptr);
    EXPECT_EQ(v.format(), VectorFormat::Flat);
    EXPECT_EQ(v.GetValue(2).GetInteger(), 42);

    SelectionVector sel(1);
    v.Slice(sel, 1);
    EXPECT_EQ(v.format(), VectorFormat::Dictionary);
    v.ReferenceFlat(Buffer::View(parent, 0, 32), ValidityMask(8), nullptr);
    EXPECT_EQ(v.format(), VectorFormat::Flat);
    v.Verify(8);
}

TEST(VectorReferenceFlat, SharedHeapKeepsStringsAlive) {
    auto heap = std::make_shared<StringHeap>();
    auto parent = Buffer::Allocate(4 * sizeof(string_t));
    for (int i = 0; i < 4; i++) {
        parent->As<string_t>()[i] = heap->Add(std::string(30 + i, 'a' + i));
    }
    Vector v(LogicalType::Varchar(), 4);
    v.ReferenceFlat(Buffer::View(parent, 0, parent->size()), ValidityMask(4), heap);
    heap.reset();
    parent.reset(); // only the vector keeps the data and heap alive now
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(v.GetValue(i).GetVarchar(), std::string(30 + i, 'a' + i));
    v.Reset(); // detaching from a shared/sealed-style heap must not clear it for others
}

TEST(VectorReferenceFlatDeathTest, ContractViolationsAbort) {
    Vector v(LogicalType::BigInt(), 64);
    auto too_small = Buffer::Allocate(10);
    EXPECT_DEATH(v.ReferenceFlat(too_small, ValidityMask(64), nullptr), "CDB_CHECK");
    auto ok = Buffer::Allocate(64 * 8);
    EXPECT_DEATH(v.ReferenceFlat(ok, ValidityMask(8), nullptr), "CDB_CHECK"); // mask too small
}

TEST(VectorCopyRows, SourceOffsetSelectsAWindowOfSourceRows) {
    test::Rng rng(11);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 200);
        Vector src = MakeFlat(t, vals, 256);
        for (auto [offset, count] :
             {std::pair<idx_t, idx_t>{0, 200}, {17, 100}, {199, 1}, {200, 0}}) {
            Vector dst(t, 256);
            VectorOps::CopyRows(src, nullptr, offset, count, dst.FlatBytes(), dst.Validity(),
                                t.id() == TypeId::Varchar ? &dst.Heap() : nullptr, 3);
            for (idx_t i = 0; i < count; i++) {
                ASSERT_TRUE(BitIdentical(dst.GetValue(3 + i), vals[offset + i]))
                    << t.ToString() << " offset=" << offset << " row " << i;
            }
        }
    }
}

TEST(VectorCopyRows, SourceOffsetAppliesToTheSelectionEntries) {
    test::Rng rng(12);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 100);
        Vector src = MakeFlat(t, vals, 128);
        SelectionVector sel = MakeSel({5, 99, 0, 42, 42, 7});
        const std::vector<sel_t> picks = {5, 99, 0, 42, 42, 7};
        Vector dst(t, 128);
        // process selection entries [2, 6): rows 0, 42, 42, 7
        VectorOps::CopyRows(src, &sel, 2, 4, dst.FlatBytes(), dst.Validity(),
                            t.id() == TypeId::Varchar ? &dst.Heap() : nullptr, 0);
        for (idx_t i = 0; i < 4; i++) {
            ASSERT_TRUE(BitIdentical(dst.GetValue(i), vals[picks[2 + i]])) << t.ToString();
        }
    }
}

TEST(VectorCopyRows, ThroughDictionaryAndConstantSourcesWithOffset) {
    test::Rng rng(13);
    for (LogicalType t : AllTypes()) {
        auto vals = RandomValues(rng, t, 64);
        Vector dict = MakeFlat(t, vals, 64);
        dict.Slice(MakeSel({63, 1, 2, 3, 62}), 5);
        Vector dst(t, 64);
        VectorOps::CopyRows(dict, nullptr, 1, 3, dst.FlatBytes(), dst.Validity(),
                            t.id() == TypeId::Varchar ? &dst.Heap() : nullptr, 0);
        ExpectEquals(dst, {vals[1], vals[2], vals[3]});

        const Value c = test::RandomValue(rng, t, 0.3);
        Vector konst = Vector::MakeConstant(c, 64);
        Vector dst2(t, 64);
        VectorOps::CopyRows(konst, nullptr, 40, 10, dst2.FlatBytes(), dst2.Validity(),
                            t.id() == TypeId::Varchar ? &dst2.Heap() : nullptr, 0);
        for (idx_t i = 0; i < 10; i++)
            ASSERT_TRUE(BitIdentical(dst2.GetValue(i), c));
    }
}

} // namespace cdb
