#include "storage/column_stats.h"

#include "storage_test_util.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

using test::AllOps;
using test::Satisfies;

// Builds the raw arrays ComputeColumnStats consumes from a list of Values.
struct Column {
    LogicalType type;
    Vector vec;
    idx_t n;
    Column(LogicalType t, const std::vector<Value>& vals)
        : type(t), vec(t, std::max<idx_t>(vals.size(), 1)), n(vals.size()) {
        for (idx_t i = 0; i < vals.size(); i++)
            vec.SetValue(i, vals[i]);
    }
    ColumnStats Stats() const {
        return ComputeColumnStats(type, vec.FlatBytes(), vec.Validity(), n);
    }
};

Value I(int32_t v) {
    return Value::Integer(v);
}
Value NullI() {
    return Value::Null(LogicalType::Integer());
}

} // namespace

TEST(ColumnStats, IntegerBoundsAndCounts) {
    Column c(LogicalType::Integer(), {I(5), NullI(), I(-3), I(17), NullI(), I(5)});
    ColumnStats s = c.Stats();
    EXPECT_EQ(s.count, 6u);
    EXPECT_EQ(s.null_count, 2u);
    ASSERT_TRUE(s.min.has_value());
    EXPECT_EQ(s.min->GetInteger(), -3);
    EXPECT_EQ(s.max->GetInteger(), 17);
    EXPECT_FALSE(s.AllNull());
}

TEST(ColumnStats, NullsDoNotParticipateInBounds) {
    // The slots under NULLs hold garbage (here: huge values); bounds must ignore them.
    Vector v(LogicalType::Integer(), 4);
    v.FlatData<int32_t>()[0] = 2000000000;
    v.FlatData<int32_t>()[1] = 10;
    v.FlatData<int32_t>()[2] = -2000000000;
    v.FlatData<int32_t>()[3] = 20;
    v.Validity().SetInvalid(0);
    v.Validity().SetInvalid(2);
    ColumnStats s = ComputeColumnStats(LogicalType::Integer(), v.FlatBytes(), v.Validity(), 4);
    EXPECT_EQ(s.min->GetInteger(), 10);
    EXPECT_EQ(s.max->GetInteger(), 20);
}

TEST(ColumnStats, AllNullHasNoBounds) {
    Column c(LogicalType::BigInt(),
             {Value::Null(LogicalType::BigInt()), Value::Null(LogicalType::BigInt())});
    ColumnStats s = c.Stats();
    EXPECT_TRUE(s.AllNull());
    EXPECT_FALSE(s.min.has_value());
    EXPECT_FALSE(s.max.has_value());
    for (CompareOp op : AllOps()) {
        EXPECT_TRUE(s.CanSkip(op, Value::BigInt(1))); // NULL never satisfies a comparison
    }
}

TEST(ColumnStats, EmptySegment) {
    Column c(LogicalType::Integer(), {});
    ColumnStats s = c.Stats();
    EXPECT_EQ(s.count, 0u);
    EXPECT_TRUE(s.AllNull()); // vacuously
    EXPECT_TRUE(s.CanSkip(CompareOp::Eq, I(1)));
}

TEST(ColumnStats, AllTypesProduceTypedBounds) {
    Column b(LogicalType::Boolean(), {Value::Boolean(true), Value::Boolean(false)});
    EXPECT_EQ(b.Stats().min->GetBoolean(), false);
    EXPECT_EQ(b.Stats().max->GetBoolean(), true);

    Column d(LogicalType::Date(), {Value::Date(date_t{100}), Value::Date(date_t{-5})});
    EXPECT_EQ(d.Stats().min->GetDate().days, -5);
    EXPECT_EQ(d.Stats().max->GetDate().days, 100);
    EXPECT_EQ(d.Stats().min->type(), LogicalType::Date());

    Column l(LogicalType::BigInt(), {Value::BigInt(INT64_MIN), Value::BigInt(INT64_MAX)});
    EXPECT_EQ(l.Stats().min->GetBigInt(), INT64_MIN);
    EXPECT_EQ(l.Stats().max->GetBigInt(), INT64_MAX);

    Column s(LogicalType::Varchar(), {Value::Varchar("pear"), Value::Varchar("apple"),
                                      Value::Varchar("zebra"), Value::Varchar("")});
    EXPECT_EQ(s.Stats().min->GetVarchar(), "");
    EXPECT_EQ(s.Stats().max->GetVarchar(), "zebra");
}

TEST(ColumnStats, DoubleBoundsUseTotalOrderWithNanLast) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Column c(LogicalType::Double(), {Value::Double(3.5), Value::Double(nan), Value::Double(-2.0),
                                     Value::Double(-0.0), Value::Double(0.0)});
    ColumnStats s = c.Stats();
    EXPECT_EQ(s.min->GetDouble(), -2.0);
    EXPECT_TRUE(std::isnan(s.max->GetDouble())); // NaN is the greatest value

    Column no_nan(LogicalType::Double(), {Value::Double(1.0), Value::Double(-1.0)});
    EXPECT_EQ(no_nan.Stats().max->GetDouble(), 1.0);

    Column only_nan(LogicalType::Double(), {Value::Double(nan), Value::Double(nan)});
    EXPECT_TRUE(std::isnan(only_nan.Stats().min->GetDouble()));
}

TEST(ColumnStats, VarcharComparesBytewiseUnsigned) {
    Column c(LogicalType::Varchar(),
             {Value::Varchar("a"), Value::Varchar("\xff"), Value::Varchar("\x01")});
    EXPECT_EQ(c.Stats().min->GetVarchar(), "\x01");
    EXPECT_EQ(c.Stats().max->GetVarchar(), "\xff");
}

TEST(ColumnStats, VeryLongStringsDisableBoundsButKeepCounts) {
    const std::string at_limit(ColumnStats::kMaxBoundStringLength, 'x');
    Column ok(LogicalType::Varchar(), {Value::Varchar(at_limit), Value::Varchar("a")});
    EXPECT_TRUE(ok.Stats().min.has_value());

    Column c(LogicalType::Varchar(), {Value::Varchar("a"), Value::Varchar(at_limit + "y"),
                                      Value::Null(LogicalType::Varchar())});
    ColumnStats s = c.Stats();
    EXPECT_EQ(s.count, 3u);
    EXPECT_EQ(s.null_count, 1u);
    EXPECT_FALSE(s.min.has_value());
    EXPECT_FALSE(s.CanSkip(CompareOp::Eq, Value::Varchar("zzz"))); // never pruned
    EXPECT_TRUE(s.CanSkip(CompareOp::Eq, Value::Null(LogicalType::Varchar())));
}

TEST(ColumnStats, LongStringUnderANullDoesNotDisableBounds) {
    Vector v(LogicalType::Varchar(), 3);
    v.SetValue(0, Value::Varchar(std::string(500, 'q')));
    v.SetValue(1, Value::Varchar("b"));
    v.SetValue(2, Value::Varchar("a"));
    v.Validity().SetInvalid(0); // the long string is hidden under a NULL
    ColumnStats s = ComputeColumnStats(LogicalType::Varchar(), v.FlatBytes(), v.Validity(), 3);
    ASSERT_TRUE(s.min.has_value());
    EXPECT_EQ(s.min->GetVarchar(), "a");
    EXPECT_EQ(s.max->GetVarchar(), "b");
}

TEST(ColumnStats, CanSkipTableForRange10To20) {
    Column c(LogicalType::Integer(), {I(10), I(20), I(15), NullI()});
    ColumnStats s = c.Stats();
    struct Case {
        CompareOp op;
        int32_t k;
        bool skip;
    };
    const Case cases[] = {
        {CompareOp::Eq, 5, true},   {CompareOp::Eq, 10, false}, {CompareOp::Eq, 17, false},
        {CompareOp::Eq, 20, false}, {CompareOp::Eq, 25, true},  {CompareOp::Ne, 10, false},
        {CompareOp::Lt, 10, true},  {CompareOp::Lt, 11, false}, {CompareOp::Lt, 5, true},
        {CompareOp::Le, 9, true},   {CompareOp::Le, 10, false}, {CompareOp::Gt, 20, true},
        {CompareOp::Gt, 19, false}, {CompareOp::Gt, 25, true},  {CompareOp::Ge, 21, true},
        {CompareOp::Ge, 20, false}, {CompareOp::Ge, 5, false},
    };
    for (const Case& k : cases) {
        EXPECT_EQ(s.CanSkip(k.op, I(k.k)), k.skip)
            << "col <" << CompareOpName(k.op) << "> " << k.k << " on [10,20]";
    }
}

TEST(ColumnStats, NeSkipsOnlyWhenEveryValueEqualsTheConstant) {
    Column same(LogicalType::Integer(), {I(7), I(7), NullI(), I(7)});
    EXPECT_TRUE(same.Stats().CanSkip(CompareOp::Ne, I(7)));
    EXPECT_FALSE(same.Stats().CanSkip(CompareOp::Ne, I(8)));
    Column mixed(LogicalType::Integer(), {I(7), I(8)});
    EXPECT_FALSE(mixed.Stats().CanSkip(CompareOp::Ne, I(7)));
}

TEST(ColumnStats, NullConstantAlwaysSkips) {
    Column c(LogicalType::Integer(), {I(1), I(2)});
    for (CompareOp op : AllOps())
        EXPECT_TRUE(c.Stats().CanSkip(op, NullI()));
}

TEST(ColumnStats, IsNullHelpers) {
    Column none(LogicalType::Integer(), {I(1), I(2)});
    EXPECT_TRUE(none.Stats().CanSkipIsNull());
    EXPECT_FALSE(none.Stats().CanSkipIsNotNull());
    Column some(LogicalType::Integer(), {I(1), NullI()});
    EXPECT_FALSE(some.Stats().CanSkipIsNull());
    EXPECT_FALSE(some.Stats().CanSkipIsNotNull());
    Column all(LogicalType::Integer(), {NullI(), NullI()});
    EXPECT_FALSE(all.Stats().CanSkipIsNull());
    EXPECT_TRUE(all.Stats().CanSkipIsNotNull());
}

TEST(ColumnStats, DoubleSkipsAgreeWithTotalOrder) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    Column c(LogicalType::Double(), {Value::Double(1.0), Value::Double(2.0)});
    ColumnStats s = c.Stats();
    EXPECT_TRUE(s.CanSkip(CompareOp::Gt, Value::Double(nan)));  // nothing exceeds NaN
    EXPECT_TRUE(s.CanSkip(CompareOp::Eq, Value::Double(nan)));  // NaN is above max
    EXPECT_FALSE(s.CanSkip(CompareOp::Lt, Value::Double(nan))); // everything is below NaN
    EXPECT_TRUE(s.CanSkip(CompareOp::Gt, Value::Double(inf)));
    EXPECT_FALSE(s.CanSkip(CompareOp::Lt, Value::Double(inf)));
}

// Soundness: if CanSkip says "no row can match", brute force over the rows must agree. And for
// the monotone operators the answer must be exact (not merely sound), so pruning is not
// needlessly timid.
TEST(ColumnStats, CanSkipIsSoundAndExactForRangeOpsOverRandomData) {
    test::Rng rng(77);
    size_t skipped = 0, kept = 0;
    for (LogicalType type : test::AllTypes()) {
        for (int trial = 0; trial < 150; trial++) {
            const idx_t n = test::RandBelow(rng, 40);
            const double null_prob = trial % 5 == 0 ? 0.9 : 0.2;
            std::vector<Value> vals;
            for (idx_t i = 0; i < n; i++)
                vals.push_back(test::RandomValue(rng, type, null_prob));
            // Strings are drawn short here so bounds stay available most of the time.
            Column c(type, vals);
            ColumnStats s = c.Stats();
            for (int k = 0; k < 12; k++) {
                const Value constant = test::RandomValue(rng, type, 0.1);
                for (CompareOp op : AllOps()) {
                    bool any = false;
                    for (const Value& v : vals)
                        any |= Satisfies(v, op, constant);
                    const bool skip = s.CanSkip(op, constant);
                    if (skip) {
                        ASSERT_FALSE(any) << type.ToString() << " " << CompareOpName(op) << " "
                                          << constant.ToString() << " skipped but a row matches";
                        skipped++;
                    } else {
                        kept++;
                    }
                    const bool bounds = s.min.has_value();
                    const bool exact_op = op == CompareOp::Lt || op == CompareOp::Le ||
                                          op == CompareOp::Gt || op == CompareOp::Ge ||
                                          op == CompareOp::Ne;
                    if (bounds && !constant.IsNull() && exact_op) {
                        ASSERT_EQ(skip, !any)
                            << type.ToString() << " " << CompareOpName(op) << " "
                            << constant.ToString() << " (pruning should be exact)";
                    }
                }
            }
        }
    }
    EXPECT_GT(skipped, 1000u); // the test must actually exercise both outcomes
    EXPECT_GT(kept, 1000u);
}

TEST(ColumnStatsDeathTest, ConstantOfTheWrongTypeAborts) {
    Column c(LogicalType::Integer(), {I(1)});
    EXPECT_DEATH(c.Stats().CanSkip(CompareOp::Eq, Value::BigInt(1)), "CDB_CHECK");
}

} // namespace cdb
