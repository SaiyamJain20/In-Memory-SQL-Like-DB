// Tests for the cardinality estimator: the rules on hand-built estimates (so every number can be
// checked by arithmetic), then the whole thing against the true row counts of random tables.

#include "planner/cardinality.h"

#include "main/connection.h"
#include "planner/optimizer.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cdb {

namespace {

using test::RandBelow;
using test::Rng;

BoundExprPtr Col(idx_t i, LogicalType t = LogicalType::Integer()) {
    return BoundExpr::ColumnRef(i, t);
}
BoundExprPtr Int(int32_t v) {
    return BoundExpr::Constant(Value::Integer(v));
}
BoundExprPtr Cmp(OperatorKind op, BoundExprPtr a, BoundExprPtr b) {
    return BoundExpr::Binary(op, std::move(a), std::move(b), LogicalType::Boolean());
}
BoundExprPtr And(BoundExprPtr a, BoundExprPtr b) {
    return Cmp(OperatorKind::And, std::move(a), std::move(b));
}
BoundExprPtr Or(BoundExprPtr a, BoundExprPtr b) {
    return Cmp(OperatorKind::Or, std::move(a), std::move(b));
}

ColumnEstimate IntColumn(double distinct, int lo, int hi, double nulls = 0) {
    ColumnEstimate c;
    c.distinct = distinct;
    c.null_fraction = nulls;
    c.min = Value::Integer(lo);
    c.max = Value::Integer(hi);
    return c;
}

// 1000 rows: a is uniform on 0..99 (100 values), b on 0..9 (10 values, 20% NULL), s is text.
Estimate Thousand() {
    Estimate e;
    e.rows = 1000;
    e.columns = {IntColumn(100, 0, 99), IntColumn(10, 0, 9, 0.2)};
    ColumnEstimate s;
    s.distinct = 50;
    s.min = Value::Varchar("apple");
    s.max = Value::Varchar("zebra");
    e.columns.push_back(s);
    return e;
}

double Sel(const BoundExpr& e) {
    return Selectivity(e, Thousand());
}

} // namespace

TEST(CardinalityRules, NumericValuesAndColumnReferencesUnderCasts) {
    EXPECT_EQ(NumericValue(Value::Integer(-7)), -7.0);
    EXPECT_EQ(NumericValue(Value::BigInt(1LL << 40)), static_cast<double>(1LL << 40));
    EXPECT_EQ(NumericValue(Value::Double(2.5)), 2.5);
    EXPECT_EQ(NumericValue(Value::Date(date_t{19000})), 19000.0);
    EXPECT_EQ(NumericValue(Value::Boolean(true)), 1.0);
    EXPECT_FALSE(NumericValue(Value::Varchar("x")).has_value());
    EXPECT_FALSE(NumericValue(Value::Null(LogicalType::Integer())).has_value());
    EXPECT_FALSE(NumericValue(Value::Double(std::numeric_limits<double>::quiet_NaN())).has_value());

    const BoundExprPtr cast =
        BoundExpr::Cast(BoundExpr::Cast(Col(3), LogicalType::BigInt()), LogicalType::Double());
    ASSERT_NE(AsColumnRef(*cast), nullptr);
    EXPECT_EQ(AsColumnRef(*cast)->ordinal, 3U);
    EXPECT_EQ(AsColumnRef(*Int(1)), nullptr);
    EXPECT_EQ(AsColumnRef(*Cmp(OperatorKind::Add, Col(0), Int(1))), nullptr);
}

TEST(CardinalityRules, EqualityIsOneOverTheDistinctValuesOfTheNonNullRows) {
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Eq, Col(0), Int(5))), 1.0 / 100, 1e-12);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Eq, Int(5), Col(0))), 1.0 / 100, 1e-12) << "either order";
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Eq, Col(1), Int(5))), 0.8 / 10, 1e-12)
        << "20% NULLs never match";
    // outside the bounds: nothing
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Eq, Col(0), Int(100))), 0.0);
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Eq, Col(0), Int(-1))), 0.0);
    EXPECT_GT(Sel(*Cmp(OperatorKind::Eq, Col(0), Int(99))), 0.0);
    // NULL constant: nothing; a text constant outside the bounds: nothing
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Eq, Col(0),
                       BoundExpr::Constant(Value::Null(LogicalType::Integer())))),
              0.0);
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Eq, Col(2, LogicalType::Varchar()),
                       BoundExpr::Constant(Value::Varchar("zzz")))),
              0.0);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Eq, Col(2, LogicalType::Varchar()),
                         BoundExpr::Constant(Value::Varchar("mango")))),
                1.0 / 50, 1e-12);
    // not equal: everything else that is not NULL
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Ne, Col(0), Int(5))), 1.0 - 1.0 / 100, 1e-12);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Ne, Col(1), Int(5))), 0.8 - 0.08, 1e-12);
}

TEST(CardinalityRules, RangesInterpolateBetweenTheBounds) {
    // a holds the 100 integers 0..99, each equally often: the answers are exact
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Lt, Col(0), Int(50))), 0.50, 1e-12);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Le, Col(0), Int(50))), 0.51, 1e-12);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Gt, Col(0), Int(50))), 0.49, 1e-12);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Ge, Col(0), Int(50))), 0.50, 1e-12);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Gt, Col(0), Int(98))), 0.01, 1e-12) << "only 99 is above 98";
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Ge, Col(0), Int(99))), 0.01, 1e-12);
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Gt, Col(0), Int(99))), 0.0);
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Lt, Col(0), Int(0))), 0.0);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Le, Col(0), Int(0))), 0.01, 1e-12);
    // a dense integer range with a slightly noisy distinct count (a sketch's 50.3 for 50 values)
    // is still the 50 integers: `qty > 49` keeps one value in fifty, not nothing
    Estimate qty;
    qty.rows = 1000;
    qty.columns = {IntColumn(50.3, 1, 50)};
    EXPECT_NEAR(Selectivity(*Cmp(OperatorKind::Gt, Col(0), Int(49)), qty), 0.02, 1e-12);
    EXPECT_NEAR(Selectivity(*Cmp(OperatorKind::Le, Col(0), Int(49)), qty), 0.98, 1e-12);
    // a sparse integer column (100 values spread over 0..9999): a grid of 100 points
    Estimate sparse;
    sparse.rows = 1000;
    sparse.columns = {IntColumn(100, 0, 9999)};
    EXPECT_NEAR(Selectivity(*Cmp(OperatorKind::Lt, Col(0), Int(5000)), sparse), 0.505, 0.006);
    // a small grid of doubles, like a discount of 0.00 .. 0.10 in steps of 0.01 (11 values)
    Estimate discount;
    discount.rows = 1000;
    ColumnEstimate d;
    d.distinct = 11;
    d.min = Value::Double(0.0);
    d.max = Value::Double(0.10);
    discount.columns = {d};
    const auto dbl = [](double k) { return BoundExpr::Constant(Value::Double(k)); };
    EXPECT_NEAR(
        Selectivity(*Cmp(OperatorKind::Le, Col(0, LogicalType::Double()), dbl(0.07)), discount),
        8.0 / 11, 1e-9);
    EXPECT_NEAR(
        Selectivity(*Cmp(OperatorKind::Ge, Col(0, LogicalType::Double()), dbl(0.05)), discount),
        6.0 / 11, 1e-9);
    EXPECT_NEAR(
        Selectivity(*Cmp(OperatorKind::Lt, Col(0, LogicalType::Double()), dbl(0.045)), discount),
        5.0 / 11, 1e-9);
    // the flipped form means the opposite direction
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Gt, Int(50), Col(0))),
                Sel(*Cmp(OperatorKind::Lt, Col(0), Int(50))), 1e-12);
    // beyond the bounds: all or nothing
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Lt, Col(0), Int(0))), 0.0);
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Lt, Col(0), Int(1000))), 1.0);
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Gt, Col(0), Int(1000))), 0.0);
    EXPECT_EQ(Sel(*Cmp(OperatorKind::Ge, Col(0), Int(-5))), 1.0);
    // NULLs never satisfy a range
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Ge, Col(1), Int(-5))), 0.8, 1e-12);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Lt, Col(1), Int(1000))), 0.8, 1e-12);
    // text: only the extremes tell; inside them a third
    const auto text = [](OperatorKind op, const char* k) {
        return Sel(
            *Cmp(op, Col(2, LogicalType::Varchar()), BoundExpr::Constant(Value::Varchar(k))));
    };
    EXPECT_EQ(text(OperatorKind::Lt, "aaa"), 0.0);
    EXPECT_EQ(text(OperatorKind::Gt, "zzz"), 0.0);
    EXPECT_EQ(text(OperatorKind::Lt, "zzz"), 1.0);
    EXPECT_EQ(text(OperatorKind::Ge, "aaa"), 1.0);
    EXPECT_NEAR(text(OperatorKind::Lt, "mango"), 1.0 / 3, 1e-12);
    // a column with no known bounds: a third
    Estimate unknown = Thousand();
    unknown.columns[0].min.reset();
    unknown.columns[0].max.reset();
    EXPECT_NEAR(Selectivity(*Cmp(OperatorKind::Lt, Col(0), Int(5)), unknown), 1.0 / 3, 1e-12);
    // a constant column
    Estimate flat = Thousand();
    flat.columns[0] = IntColumn(1, 7, 7);
    EXPECT_EQ(Selectivity(*Cmp(OperatorKind::Lt, Col(0), Int(8)), flat), 1.0);
    EXPECT_EQ(Selectivity(*Cmp(OperatorKind::Gt, Col(0), Int(8)), flat), 0.0);
}

TEST(CardinalityRules, ALowerAndAnUpperBoundOnOneColumnAreAnIntervalNotTwoIndependentShares) {
    // a: the integers 0..99. `a >= 20 AND a < 30` is ten values, not 0.8 * 0.3
    EXPECT_NEAR(
        Sel(*And(Cmp(OperatorKind::Ge, Col(0), Int(20)), Cmp(OperatorKind::Lt, Col(0), Int(30)))),
        0.10, 1e-12);
    EXPECT_NEAR(
        Sel(*And(Cmp(OperatorKind::Gt, Col(0), Int(20)), Cmp(OperatorKind::Le, Col(0), Int(30)))),
        0.10, 1e-12);
    EXPECT_NEAR(
        Sel(*And(Cmp(OperatorKind::Ge, Col(0), Int(20)), Cmp(OperatorKind::Le, Col(0), Int(30)))),
        0.11, 1e-12);
    // the bounds in either order, and the constant on the left
    EXPECT_NEAR(
        Sel(*And(Cmp(OperatorKind::Lt, Col(0), Int(30)), Cmp(OperatorKind::Ge, Col(0), Int(20)))),
        0.10, 1e-12);
    EXPECT_NEAR(
        Sel(*And(Cmp(OperatorKind::Le, Int(20), Col(0)), Cmp(OperatorKind::Gt, Int(30), Col(0)))),
        0.10, 1e-12);
    // an empty interval, and one wider than the data
    EXPECT_EQ(
        Sel(*And(Cmp(OperatorKind::Ge, Col(0), Int(50)), Cmp(OperatorKind::Lt, Col(0), Int(40)))),
        0.0);
    EXPECT_NEAR(
        Sel(*And(Cmp(OperatorKind::Ge, Col(0), Int(-10)), Cmp(OperatorKind::Lt, Col(0), Int(500)))),
        1.0, 1e-12);
    // NULLs are in no interval: b (0..9, 20% NULL) in [2, 4]
    EXPECT_NEAR(
        Sel(*And(Cmp(OperatorKind::Ge, Col(1), Int(2)), Cmp(OperatorKind::Le, Col(1), Int(4)))),
        0.8 * 0.3, 1e-12);
    // two columns: one interval each, independent of one another
    EXPECT_NEAR(
        Sel(*And(
            And(Cmp(OperatorKind::Ge, Col(0), Int(20)), Cmp(OperatorKind::Lt, Col(0), Int(30))),
            And(Cmp(OperatorKind::Ge, Col(1), Int(2)), Cmp(OperatorKind::Le, Col(1), Int(4))))),
        0.10 * 0.24, 1e-12);
    // a third conjunct that is not a range is multiplied in
    EXPECT_NEAR(Sel(*And(And(Cmp(OperatorKind::Ge, Col(0), Int(20)),
                             Cmp(OperatorKind::Lt, Col(0), Int(30))),
                         Cmp(OperatorKind::Eq, Col(1), Int(3)))),
                0.10 * 0.08, 1e-12);
    // two lower bounds do not make an interval: they stay independent
    EXPECT_NEAR(
        Sel(*And(Cmp(OperatorKind::Ge, Col(0), Int(20)), Cmp(OperatorKind::Gt, Col(0), Int(40)))),
        0.8 * 0.59, 1e-12);
    // a column in a join output is a column like any other (the dates of orders: a year of 2400
    // days)
    Estimate dates;
    dates.rows = 150000;
    ColumnEstimate d;
    d.distinct = 2400;
    d.min = Value::Date(date_t{8035});
    d.max = Value::Date(date_t{10435});
    dates.columns = {d};
    const auto date = [](int32_t days) { return BoundExpr::Constant(Value::Date(date_t{days})); };
    const double year =
        Selectivity(*And(Cmp(OperatorKind::Ge, Col(0, LogicalType::Date()), date(8766)),
                         Cmp(OperatorKind::Lt, Col(0, LogicalType::Date()), date(9131))),
                    dates);
    EXPECT_NEAR(year, 365.0 / 2400, 0.002) << "a year of the seven, 15%";
    // ApplyFilter agrees, and narrows the bounds to the interval
    const Estimate out =
        ApplyFilter(dates, *And(Cmp(OperatorKind::Ge, Col(0, LogicalType::Date()), date(8766)),
                                Cmp(OperatorKind::Lt, Col(0, LogicalType::Date()), date(9131))));
    EXPECT_NEAR(out.rows, 150000 * 365.0 / 2400, 150000 * 0.002);
    EXPECT_EQ(NumericValue(*out.columns[0].min), 8766.0);
    EXPECT_EQ(NumericValue(*out.columns[0].max), 9131.0);
}

TEST(CardinalityRules, BooleanStructureNullsListsAndPatterns) {
    const double eq = 1.0 / 100;
    const auto a_eq = [](int k) { return Cmp(OperatorKind::Eq, Col(0), Int(k)); };
    EXPECT_NEAR(Sel(*And(a_eq(1), Cmp(OperatorKind::Lt, Col(0), Int(50)))), eq * 0.5, 1e-12)
        << "independence";
    EXPECT_NEAR(Sel(*Or(a_eq(1), a_eq(2))), 2 * eq - eq * eq, 1e-12);
    EXPECT_NEAR(Sel(*BoundExpr::Unary(OperatorKind::Not, a_eq(1), LogicalType::Boolean())), 1 - eq,
                1e-12);
    EXPECT_NEAR(Sel(*BoundExpr::IsNull(Col(1), false)), 0.2, 1e-12);
    EXPECT_NEAR(Sel(*BoundExpr::IsNull(Col(1), true)), 0.8, 1e-12);
    EXPECT_EQ(Sel(*BoundExpr::IsNull(Col(0), false)), 0.0);
    std::vector<BoundExprPtr> items;
    items.push_back(Int(1));
    items.push_back(Int(2));
    items.push_back(Int(3));
    EXPECT_NEAR(Sel(*BoundExpr::InList(Col(0), std::move(items), false)), 3 * eq, 1e-12);
    std::vector<BoundExprPtr> outside;
    outside.push_back(Int(500));
    outside.push_back(Int(600));
    EXPECT_EQ(Sel(*BoundExpr::InList(Col(0), std::move(outside), false)), 0.0);
    std::vector<BoundExprPtr> negated;
    negated.push_back(Int(1));
    negated.push_back(Int(2));
    EXPECT_NEAR(Sel(*BoundExpr::InList(Col(0), std::move(negated), true)), 1 - 2 * eq, 1e-12);
    // constants
    EXPECT_EQ(Sel(*BoundExpr::Constant(Value::Boolean(true))), 1.0);
    EXPECT_EQ(Sel(*BoundExpr::Constant(Value::Boolean(false))), 0.0);
    EXPECT_EQ(Sel(*BoundExpr::Constant(Value::Null(LogicalType::Boolean()))), 0.0);
    // LIKE: no wildcard is an equality, a prefix a tenth, anything else a fifth
    const auto like = [](const char* pattern) {
        std::vector<BoundExprPtr> args;
        args.push_back(Col(2, LogicalType::Varchar()));
        args.push_back(BoundExpr::Constant(Value::Varchar(pattern)));
        return Sel(*BoundExpr::Call(FunctionId::Like, std::move(args), LogicalType::Boolean()));
    };
    EXPECT_NEAR(like("mango"), 1.0 / 50, 1e-12);
    EXPECT_NEAR(like("ma%"), 0.1, 1e-12);
    EXPECT_NEAR(like("%go"), 0.2, 1e-12);
    EXPECT_NEAR(like("%an%"), 0.2, 1e-12);
    // two columns of the same input
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Eq, Col(0), Col(1))), 0.8 / 100, 1e-12);
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Lt, Col(0), Col(1))), 0.8 / 3, 1e-12);
    // anything it cannot reason about: half
    EXPECT_NEAR(Sel(*Cmp(OperatorKind::Eq, Cmp(OperatorKind::Add, Col(0), Int(1)), Int(5))), 0.1,
                1e-12);
    // every selectivity is a probability
    for (int k = -3; k < 130; k += 7) {
        for (const OperatorKind op : {OperatorKind::Eq, OperatorKind::Ne, OperatorKind::Lt,
                                      OperatorKind::Le, OperatorKind::Gt, OperatorKind::Ge}) {
            for (idx_t c : {idx_t{0}, idx_t{1}}) {
                const double s = Sel(*Cmp(op, Col(c), Int(k)));
                EXPECT_GE(s, 0.0);
                EXPECT_LE(s, 1.0);
            }
        }
    }
}

TEST(CardinalityRules, AFilterScalesTheRowsAndNarrowsWhatItConstrains) {
    const Estimate in = Thousand();
    // a = 7: one value left
    Estimate out = ApplyFilter(in, *Cmp(OperatorKind::Eq, Col(0), Int(7)));
    EXPECT_NEAR(out.rows, 10.0, 1e-9);
    EXPECT_EQ(out.columns[0].distinct, 1.0);
    EXPECT_EQ(Value::Compare(*out.columns[0].min, Value::Integer(7)), 0);
    EXPECT_EQ(Value::Compare(*out.columns[0].max, Value::Integer(7)), 0);
    EXPECT_EQ(out.columns[0].null_fraction, 0.0);
    EXPECT_LE(out.columns[1].distinct, 10.0) << "other columns cannot have more values than rows";
    // `a < 2` leaves 20 rows, so the text column (50 distinct values among 1000 rows) has at most
    // 20
    out = ApplyFilter(in, *Cmp(OperatorKind::Lt, Col(0), Int(2)));
    EXPECT_NEAR(out.rows, 20.0, 1e-9);
    EXPECT_LE(out.columns[2].distinct, 20.0 + 1e-9);
    EXPECT_GE(out.columns[2].distinct, 1.0);
    // a < 25 and a >= 10: the range [10, 24] of [0, 99]
    out = ApplyFilter(
        in, *And(Cmp(OperatorKind::Lt, Col(0), Int(25)), Cmp(OperatorKind::Ge, Col(0), Int(10))));
    EXPECT_EQ(Value::Compare(*out.columns[0].min, Value::Integer(10)), 0);
    EXPECT_EQ(Value::Compare(*out.columns[0].max, Value::Integer(25)), 0);
    // the distinct count follows the share of the range that is left: 25/99 of it after `< 25`,
    // then 15/25 of that after `>= 10`
    EXPECT_NEAR(out.columns[0].distinct, 100.0 * (25.0 / 99) * (15.0 / 25), 1e-9);
    // IS NOT NULL clears the NULL fraction
    out = ApplyFilter(in, *BoundExpr::IsNull(Col(1), true));
    EXPECT_NEAR(out.rows, 800.0, 1e-9);
    EXPECT_EQ(out.columns[1].null_fraction, 0.0);
    // IN (...) caps the distinct values
    std::vector<BoundExprPtr> items;
    items.push_back(Int(1));
    items.push_back(Int(2));
    out = ApplyFilter(in, *BoundExpr::InList(Col(0), std::move(items), false));
    EXPECT_LE(out.columns[0].distinct, 2.0);
    // an impossible filter: no rows, and the distinct counts stay >= 1 (they are denominators)
    out = ApplyFilter(in, *Cmp(OperatorKind::Eq, Col(0), Int(1000)));
    EXPECT_EQ(out.rows, 0.0);
    for (const ColumnEstimate& c : out.columns) {
        EXPECT_GE(c.distinct, 1.0);
    }
    // the input is not modified
    EXPECT_EQ(in.rows, 1000.0);
    EXPECT_EQ(in.columns[0].distinct, 100.0);
}

TEST(CardinalityRules, JoinsOfEveryKind) {
    // 1000 orders-like rows referencing 100 customer-like rows by a key with 100 values
    Estimate fk;
    fk.rows = 1000;
    fk.columns = {IntColumn(100, 0, 99), IntColumn(1000, 0, 999)};
    Estimate pk;
    pk.rows = 100;
    pk.columns = {IntColumn(100, 0, 99), IntColumn(5, 0, 4)};
    const BoundExprPtr on = Cmp(OperatorKind::Eq, Col(0), Col(2)); // fk.0 = pk.0 over left ++ right

    Estimate inner = EstimateJoin(JoinType::Inner, fk, pk, on.get());
    EXPECT_NEAR(inner.rows, 1000.0, 1e-9) << "a foreign key: every row finds its one partner";
    ASSERT_EQ(inner.columns.size(), 4U);
    EXPECT_EQ(inner.columns[0].distinct, 100.0);
    // the same join with the sides swapped is the same size
    const BoundExprPtr swapped = Cmp(OperatorKind::Eq, Col(0), Col(2));
    EXPECT_NEAR(EstimateJoin(JoinType::Inner, pk, fk, swapped.get()).rows, 1000.0, 1e-9);
    // the condition written the other way round
    const BoundExprPtr reversed = Cmp(OperatorKind::Eq, Col(2), Col(0));
    EXPECT_NEAR(EstimateJoin(JoinType::Inner, fk, pk, reversed.get()).rows, 1000.0, 1e-9);

    // a cross join is the product; a residual predicate scales it
    EXPECT_NEAR(EstimateJoin(JoinType::Cross, fk, pk, nullptr).rows, 100000.0, 1e-6);
    const BoundExprPtr range = Cmp(OperatorKind::Lt, Col(1), Col(3));
    EXPECT_LT(EstimateJoin(JoinType::Inner, fk, pk, range.get()).rows, 100000.0 / 2);

    // a left join keeps every left row, whatever the keys
    Estimate rare;
    rare.rows = 10;
    rare.columns = {IntColumn(10, 5000, 5009), IntColumn(10, 0, 9)};
    const BoundExprPtr on_rare = Cmp(OperatorKind::Eq, Col(0), Col(2));
    const Estimate left = EstimateJoin(JoinType::Left, fk, rare, on_rare.get());
    EXPECT_GE(left.rows, 1000.0);
    EXPECT_GT(left.columns[2].null_fraction, 0.0) << "the right side is padded with NULLs";

    // semi and anti split the left rows between them
    const Estimate semi = EstimateJoin(JoinType::Semi, fk, rare, on_rare.get());
    const Estimate anti = EstimateJoin(JoinType::Anti, fk, rare, on_rare.get());
    EXPECT_NEAR(semi.rows + anti.rows, 1000.0, 1e-6);
    EXPECT_EQ(semi.columns.size(), 2U) << "the left columns only";
    EXPECT_LT(semi.rows, 1000.0 * 0.2) << "10 of the 100 key values";
    EXPECT_NEAR(EstimateJoin(JoinType::AntiNullAware, fk, rare, on_rare.get()).rows, anti.rows,
                1e-9);
    // an uncorrelated EXISTS: all or nothing
    EXPECT_NEAR(EstimateJoin(JoinType::Semi, fk, pk, nullptr).rows, 1000.0, 1e-9);
    Estimate none;
    none.rows = 0;
    none.columns = pk.columns;
    EXPECT_EQ(EstimateJoin(JoinType::Semi, fk, none, nullptr).rows, 0.0);
    EXPECT_NEAR(EstimateJoin(JoinType::Anti, fk, none, nullptr).rows, 1000.0, 1e-9);

    // a composite key cannot match more than the larger side has rows
    Estimate a, b;
    a.rows = b.rows = 1000;
    a.columns = b.columns = {IntColumn(100, 0, 99), IntColumn(20, 0, 19)};
    const BoundExprPtr two =
        And(Cmp(OperatorKind::Eq, Col(0), Col(2)), Cmp(OperatorKind::Eq, Col(1), Col(3)));
    EXPECT_NEAR(EstimateJoin(JoinType::Inner, a, b, two.get()).rows, 1000.0 * 1000 / 1000, 1e-6)
        << "100 x 20 combinations, but at most 1000 are in use";
}

namespace {

struct Env {
    Database db;
    Connection conn{db};
    void Run(const std::string& sql) {
        const QueryResult r = conn.Query(sql);
        ASSERT_TRUE(r.ok()) << sql.substr(0, 150) << "\n" << r.error_message();
    }
    double Estimated(const std::string& sql) {
        LogicalPtr plan = Optimize(conn.Plan(sql));
        CardinalityEstimator estimator;
        return estimator.Rows(*plan);
    }
    double Actual(const std::string& sql) {
        const QueryResult r = conn.Query("SELECT count(*) FROM (" + sql + ") AS q");
        EXPECT_TRUE(r.ok()) << r.error_message();
        return static_cast<double>(r.GetValue(0, 0).GetBigInt());
    }
};

double QError(double estimate, double actual) {
    const double e = std::max(1.0, estimate), a = std::max(1.0, actual);
    return std::max(e / a, a / e);
}

} // namespace

TEST(CardinalityEstimates, ScansAndFiltersOfUniformDataAreCloseToTheTruth) {
    Rng rng(7);
    Env env;
    env.Run("CREATE TABLE t (id INTEGER, a INTEGER, b INTEGER, d DOUBLE, s VARCHAR, f BOOLEAN)");
    std::string values;
    for (int i = 0; i < 20000; i++) {
        values +=
            (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
            std::to_string(RandBelow(rng, 100)) + "," +
            (RandBelow(rng, 10) == 0 ? std::string("NULL") : std::to_string(RandBelow(rng, 1000))) +
            "," + std::to_string(static_cast<double>(RandBelow(rng, 100000)) / 100.0) + ",'k" +
            std::to_string(RandBelow(rng, 40)) + "'," + (RandBelow(rng, 2) ? "true" : "false") +
            ")";
    }
    env.Run("INSERT INTO t VALUES " + values);
    // a scan is exact
    EXPECT_EQ(env.Estimated("SELECT * FROM t"), 20000.0);
    std::vector<double> errors;
    const std::vector<std::string> predicates = {
        "a = 17",
        "a <> 17",
        "a < 10",
        "a >= 90",
        "a BETWEEN 20 AND 29",
        "b < 100",
        "b IS NULL",
        "b IS NOT NULL",
        "b = 500",
        "d > 900.0",
        "d < 10.0",
        "s = 'k5'",
        "s <> 'k5'",
        "s LIKE 'k1%'",
        "a IN (1, 2, 3)",
        "a NOT IN (1, 2, 3)",
        "a = 3 AND b < 500",
        "a < 50 AND d < 500.0",
        "a = 3 OR a = 4",
        "a < 5 OR b > 900",
        "NOT (a < 90)",
        "id < 5000",
        "id >= 19990",
        "a = 1000",
        "id < 0",
        "a < 30 AND a > 20",
        "b IS NOT NULL AND b < 50",
        "f",
    };
    for (const std::string& p : predicates) {
        const std::string sql = "SELECT * FROM t WHERE " + p;
        const double estimate = env.Estimated(sql), actual = env.Actual(sql);
        const double q = QError(estimate, actual);
        errors.push_back(q);
        EXPECT_LE(q, 3.0) << p << ": estimated " << estimate << ", actually " << actual;
    }
    std::sort(errors.begin(), errors.end());
    EXPECT_LE(errors[errors.size() / 2], 1.35) << "the median error";
}

TEST(CardinalityEstimates, JoinsAndAggregatesOfSyntheticSchemasAreCloseToTheTruth) {
    Rng rng(8);
    Env env;
    env.Run("CREATE TABLE cust (ck INTEGER, nation INTEGER, bal INTEGER)");
    env.Run("CREATE TABLE ord (ok INTEGER, ck INTEGER, price INTEGER)");
    env.Run("CREATE TABLE item (ok INTEGER, qty INTEGER, flag INTEGER)");
    env.Run("CREATE TABLE nat (nk INTEGER, region INTEGER)");
    std::string cust, ord, item, nat;
    for (int i = 0; i < 1000; i++) {
        cust += (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
                std::to_string(RandBelow(rng, 25)) + "," + std::to_string(RandBelow(rng, 10000)) +
                ")";
    }
    for (int i = 0; i < 10000; i++) {
        ord += (i ? "," : "") + std::string("(") + std::to_string(i) + "," +
               std::to_string(RandBelow(rng, 1000)) + "," + std::to_string(RandBelow(rng, 1000)) +
               ")";
    }
    for (int i = 0; i < 40000; i++) {
        item += (i ? "," : "") + std::string("(") + std::to_string(RandBelow(rng, 10000)) + "," +
                std::to_string(1 + RandBelow(rng, 50)) + "," + std::to_string(RandBelow(rng, 3)) +
                ")";
    }
    for (int i = 0; i < 25; i++) {
        nat += (i ? "," : "") + std::string("(") + std::to_string(i) + "," + std::to_string(i % 5) +
               ")";
    }
    env.Run("INSERT INTO cust VALUES " + cust);
    env.Run("INSERT INTO ord VALUES " + ord);
    env.Run("INSERT INTO item VALUES " + item);
    env.Run("INSERT INTO nat VALUES " + nat);
    const std::vector<std::string> queries = {
        "SELECT * FROM ord JOIN cust ON ord.ck = cust.ck",
        "SELECT * FROM item JOIN ord ON item.ok = ord.ok",
        "SELECT * FROM item, ord, cust WHERE item.ok = ord.ok AND ord.ck = cust.ck",
        "SELECT * FROM item, ord, cust, nat WHERE item.ok = ord.ok AND ord.ck = cust.ck AND "
        "cust.nation = nat.nk",
        "SELECT * FROM item, ord, cust WHERE item.ok = ord.ok AND ord.ck = cust.ck AND cust.nation "
        "= 3",
        "SELECT * FROM item, ord WHERE item.ok = ord.ok AND ord.price < 100 AND item.qty > 40",
        "SELECT * FROM ord LEFT JOIN cust ON ord.ck = cust.ck AND cust.nation = 3",
        "SELECT * FROM cust WHERE EXISTS (SELECT 1 FROM ord WHERE ord.ck = cust.ck AND ord.price < "
        "5)",
        "SELECT * FROM cust WHERE NOT EXISTS (SELECT 1 FROM ord WHERE ord.ck = cust.ck AND "
        "ord.price < 5)",
        "SELECT * FROM cust WHERE ck IN (SELECT ck FROM ord WHERE price < 100)",
        "SELECT * FROM ord WHERE ok IN (SELECT ok FROM item WHERE qty > 49)",
        "SELECT nation, count(*) FROM cust GROUP BY nation",
        "SELECT ck, count(*) FROM ord GROUP BY ck",
        "SELECT flag, qty, count(*) FROM item GROUP BY flag, qty",
        "SELECT DISTINCT nation FROM cust",
        "SELECT DISTINCT ck FROM ord WHERE price < 500",
        "SELECT sum(price) FROM ord",
        "SELECT * FROM ord ORDER BY price LIMIT 10",
        "SELECT * FROM item, ord WHERE item.ok = ord.ok AND ord.ck = 5",
    };
    for (const std::string& sql : queries) {
        const double estimate = env.Estimated(sql), actual = env.Actual(sql);
        EXPECT_LE(QError(estimate, actual), 2.5)
            << sql << ": estimated " << estimate << ", actually " << actual;
    }
}

TEST(CardinalityEstimates, TheGroupsOfAnExpressionAreBoundedByWhatTheExpressionCanReturn) {
    Rng rng(9);
    Env env;
    env.Run("CREATE TABLE ev (id INTEGER, d DATE, a INTEGER, b INTEGER, s VARCHAR)");
    std::string values;
    for (int i = 0; i < 6000; i++) {
        // dates over seven years (2000-2006), a in 0..9, b in 0..99, s one of 40 words
        const int day = static_cast<int>(RandBelow(rng, 2557));
        values += (i ? "," : "") + std::string("(") + std::to_string(i) +
                  ",DATE '2000-01-01' + INTERVAL '" + std::to_string(day) + "' DAY," +
                  std::to_string(RandBelow(rng, 10)) + "," + std::to_string(RandBelow(rng, 100)) +
                  ",'w" + std::to_string(RandBelow(rng, 40)) + "')";
    }
    env.Run("INSERT INTO ev VALUES " + values);
    struct Case {
        const char* group;
        double actual; // distinct values of the expression
        double most;   // an estimate above this is a miss
    };
    const Case cases[] = {
        {"extract(year FROM d)", 7, 14},
        {"extract(month FROM d)", 12, 12},
        {"extract(day FROM d)", 31, 31},
        {"a > 4", 2, 3},
        {"CASE WHEN a < 3 THEN 'low' WHEN a < 7 THEN 'mid' ELSE 'high' END", 3, 3},
        {"CASE WHEN a < 5 THEN 1 ELSE 0 END", 2, 2},
        {"upper(s)", 40, 48},
        {"substring(s FROM 1 FOR 2)", 4, 48},
        {"s LIKE 'w1%'", 2, 3},
        {"a + b", 109, 109 * 1.4},
        {"a - b", 109, 109 * 1.4},
        {"a * 3", 10, 14},
        {"a + a", 10, 14},
    };
    for (const Case& c : cases) {
        const std::string sql =
            std::string("SELECT ") + c.group + ", count(*) FROM ev GROUP BY " + c.group;
        const double estimate = env.Estimated(sql);
        EXPECT_LE(estimate, c.most + 1e-9) << c.group << ": estimated " << estimate << " groups";
        EXPECT_GE(estimate, c.actual * 0.5) << c.group << ": estimated " << estimate << " groups";
    }
}

TEST(CardinalityEstimates, EveryOperatorOfAPlanHasAnEstimateWithTheRightColumnCount) {
    Env env;
    env.Run("CREATE TABLE t (a INTEGER, b VARCHAR)");
    env.Run("CREATE TABLE u (a INTEGER, c DOUBLE)");
    env.Run("INSERT INTO t VALUES (1, 'x'), (2, 'y'), (3, NULL)");
    env.Run("INSERT INTO u VALUES (1, 0.5), (2, 1.5)");
    for (const std::string sql :
         {"SELECT a, b FROM t WHERE a > 1 ORDER BY b LIMIT 5",
          "SELECT t.a, c FROM t JOIN u ON t.a = u.a",
          "SELECT b, count(*), sum(a) FROM t GROUP BY b HAVING count(*) > 0",
          "SELECT DISTINCT a FROM t",
          "SELECT a FROM t WHERE a IN (SELECT a FROM u) AND a > (SELECT min(a) FROM u)",
          "SELECT (SELECT max(a) FROM u WHERE u.a = t.a) FROM t",
          "WITH x AS (SELECT a FROM t) SELECT * FROM x, u", "SELECT 1, 'x'"}) {
        LogicalPtr plan = Optimize(env.conn.Plan(sql));
        CardinalityEstimator estimator;
        std::function<void(const LogicalOperator&)> check = [&](const LogicalOperator& op) {
            const Estimate& e = estimator.Of(op);
            EXPECT_EQ(e.columns.size(), op.ColumnCount()) << sql << " at " << op.Describe();
            EXPECT_GE(e.rows, 0.0) << sql;
            for (const ColumnEstimate& c : e.columns) {
                EXPECT_GE(c.distinct, 1.0) << sql << " at " << op.Describe();
                EXPECT_GE(c.null_fraction, 0.0);
                EXPECT_LE(c.null_fraction, 1.0);
            }
            for (const auto& child : op.children) {
                check(*child);
            }
        };
        check(*plan);
    }
}

} // namespace cdb
