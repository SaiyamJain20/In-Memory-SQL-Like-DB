#include "vector/data_chunk.h"

#include "test_util.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

using test::BitIdentical;

std::vector<LogicalType> Schema() {
    return {LogicalType::Integer(), LogicalType::Varchar(), LogicalType::Double(),
            LogicalType::Date(), LogicalType::Boolean()};
}

std::vector<std::vector<Value>> FillRandom(DataChunk& chunk, test::Rng& rng, idx_t rows,
                                           idx_t at = 0) {
    std::vector<std::vector<Value>> out(chunk.ColumnCount());
    for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
        for (idx_t r = 0; r < rows; r++) {
            out[c].push_back(test::RandomValue(rng, chunk.types()[c], 0.2));
            chunk.SetValue(c, at + r, out[c].back());
        }
    }
    return out;
}

SelectionVector MakeSel(const std::vector<sel_t>& idx) {
    SelectionVector s(idx.size());
    for (idx_t i = 0; i < idx.size(); i++)
        s.Set(i, idx[i]);
    return s;
}

} // namespace

TEST(DataChunk, InitializeCreatesOneFlatVectorPerType) {
    DataChunk c;
    c.Initialize(Schema());
    EXPECT_EQ(c.ColumnCount(), 5u);
    EXPECT_EQ(c.size(), 0u);
    EXPECT_EQ(c.capacity(), kVectorSize);
    for (idx_t i = 0; i < c.ColumnCount(); i++) {
        EXPECT_EQ(c.column(i).type(), Schema()[i]);
        EXPECT_EQ(c.column(i).format(), VectorFormat::Flat);
        EXPECT_EQ(c.column(i).capacity(), kVectorSize);
    }
    c.Verify();
}

TEST(DataChunk, EmptySchemaIsValid) {
    DataChunk c;
    c.Initialize({});
    EXPECT_EQ(c.ColumnCount(), 0u);
    c.SetCardinality(0);
    c.Verify();
}

TEST(DataChunk, SetGetAndCardinality) {
    test::Rng rng(1);
    DataChunk c;
    c.Initialize(Schema(), 100);
    auto expect = FillRandom(c, rng, 100);
    c.SetCardinality(100);
    EXPECT_EQ(c.size(), 100u);
    for (idx_t col = 0; col < c.ColumnCount(); col++) {
        for (idx_t r = 0; r < 100; r++) {
            ASSERT_TRUE(BitIdentical(c.GetValue(col, r), expect[col][r])) << col << "," << r;
        }
    }
    c.Verify();
}

TEST(DataChunk, AppendConcatenatesRowsAcrossCalls) {
    test::Rng rng(2);
    DataChunk a, b, dst;
    a.Initialize(Schema(), 100);
    b.Initialize(Schema(), 100);
    dst.Initialize(Schema(), 300);
    auto ea = FillRandom(a, rng, 70);
    a.SetCardinality(70);
    auto eb = FillRandom(b, rng, 100);
    b.SetCardinality(100);

    dst.Append(a);
    dst.Append(b);
    dst.Append(a);
    EXPECT_EQ(dst.size(), 240u);
    for (idx_t col = 0; col < dst.ColumnCount(); col++) {
        for (idx_t r = 0; r < 240; r++) {
            const Value& expect =
                r < 70 ? ea[col][r] : (r < 170 ? eb[col][r - 70] : ea[col][r - 170]);
            ASSERT_TRUE(BitIdentical(dst.GetValue(col, r), expect)) << col << "," << r;
        }
    }
    dst.Verify();
}

TEST(DataChunk, AppendEmptyChunkIsANoOp) {
    DataChunk a, dst;
    a.Initialize(Schema(), 10);
    dst.Initialize(Schema(), 10);
    dst.Append(a);
    EXPECT_EQ(dst.size(), 0u);
}

TEST(DataChunk, AppendAfterSliceMaterialisesFirst) {
    test::Rng rng(3);
    DataChunk src, dst, extra;
    src.Initialize(Schema(), 50);
    auto es = FillRandom(src, rng, 50);
    src.SetCardinality(50);
    extra.Initialize(Schema(), 50);
    auto ee = FillRandom(extra, rng, 10);
    extra.SetCardinality(10);

    dst.Initialize(Schema(), 50);
    dst.Append(src);
    dst.Slice(MakeSel({49, 0, 25}), 3); // dst columns are now dictionary vectors
    dst.Append(extra);                  // must flatten, then append
    ASSERT_EQ(dst.size(), 13u);
    for (idx_t col = 0; col < dst.ColumnCount(); col++) {
        EXPECT_TRUE(BitIdentical(dst.GetValue(col, 0), es[col][49]));
        EXPECT_TRUE(BitIdentical(dst.GetValue(col, 1), es[col][0]));
        EXPECT_TRUE(BitIdentical(dst.GetValue(col, 2), es[col][25]));
        for (idx_t r = 0; r < 10; r++) {
            EXPECT_TRUE(BitIdentical(dst.GetValue(col, 3 + r), ee[col][r]));
        }
    }
    dst.Verify();
}

TEST(DataChunk, SliceAppliesToEveryColumnAndSetsCardinality) {
    test::Rng rng(4);
    DataChunk c;
    c.Initialize(Schema(), 64);
    auto expect = FillRandom(c, rng, 64);
    c.SetCardinality(64);
    const std::vector<sel_t> idx = {63, 1, 1, 0, 40};
    c.Slice(MakeSel(idx), idx.size());
    EXPECT_EQ(c.size(), 5u);
    for (idx_t col = 0; col < c.ColumnCount(); col++) {
        EXPECT_EQ(c.column(col).format(), VectorFormat::Dictionary);
        for (idx_t r = 0; r < idx.size(); r++) {
            ASSERT_TRUE(BitIdentical(c.GetValue(col, r), expect[col][idx[r]]));
        }
    }
    c.Verify();
}

TEST(DataChunk, FlattenAfterSliceKeepsRows) {
    test::Rng rng(5);
    DataChunk c;
    c.Initialize(Schema(), 32);
    auto expect = FillRandom(c, rng, 32);
    c.SetCardinality(32);
    c.Slice(MakeSel({31, 15, 15, 0}), 4);
    c.Flatten();
    for (idx_t col = 0; col < c.ColumnCount(); col++) {
        EXPECT_EQ(c.column(col).format(), VectorFormat::Flat);
        EXPECT_TRUE(BitIdentical(c.GetValue(col, 0), expect[col][31]));
        EXPECT_TRUE(BitIdentical(c.GetValue(col, 3), expect[col][0]));
    }
    c.Verify();
}

TEST(DataChunk, ResetAllowsRefillingManyTimes) {
    test::Rng rng(6);
    DataChunk c;
    c.Initialize(Schema(), 128);
    for (int round = 0; round < 25; round++) {
        c.Reset();
        EXPECT_EQ(c.size(), 0u);
        auto expect = FillRandom(c, rng, 128);
        c.SetCardinality(128);
        if (round % 3 == 0)
            c.Slice(MakeSel({127, 0}), 2); // leave it dictionary-shaped sometimes
        const idx_t n = c.size();
        for (idx_t col = 0; col < c.ColumnCount(); col++) {
            for (idx_t r = 0; r < n; r++) {
                const idx_t src = (round % 3 == 0) ? (r == 0 ? 127 : 0) : r;
                ASSERT_TRUE(BitIdentical(c.GetValue(col, r), expect[col][src]));
            }
        }
        c.Verify();
    }
}

TEST(DataChunk, ToStringShowsRowsAndNulls) {
    DataChunk c;
    c.Initialize({LogicalType::Integer(), LogicalType::Varchar()}, 4);
    c.SetValue(0, 0, Value::Integer(1));
    c.SetValue(1, 0, Value::Varchar("one"));
    c.SetValue(0, 1, Value::Null(LogicalType::Integer()));
    c.SetValue(1, 1, Value::Varchar("two"));
    c.SetCardinality(2);
    const std::string s = c.ToString();
    EXPECT_NE(s.find("2 rows"), std::string::npos);
    EXPECT_NE(s.find("1 | one"), std::string::npos);
    EXPECT_NE(s.find("NULL | two"), std::string::npos);
}

TEST(DataChunkDeathTest, ContractViolationsAbort) {
    DataChunk c;
    c.Initialize({LogicalType::Integer()}, 4);
    EXPECT_DEATH(c.SetCardinality(5), "CDB_CHECK");
    EXPECT_DEATH(c.GetValue(0, 0), "CDB_CHECK"); // row >= size()

    DataChunk other;
    other.Initialize({LogicalType::BigInt()}, 4);
    EXPECT_DEATH(c.Append(other), "CDB_CHECK"); // schema mismatch

    DataChunk big;
    big.Initialize({LogicalType::Integer()}, 4);
    big.SetCardinality(4);
    c.SetCardinality(1);
    EXPECT_DEATH(c.Append(big), "CDB_CHECK"); // would exceed capacity
}

} // namespace cdb
