#include "planner/bound_expression.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

using E = BoundExprPtr;

E Col(idx_t i, LogicalType t = LogicalType::Integer(), const char* name = "") {
    return BoundExpr::ColumnRef(i, t, name);
}
E I(int32_t v) {
    return BoundExpr::Constant(Value::Integer(v));
}

E Sample() {
    // ((#0 + 1) > 5) AND (s LIKE 'a%')
    E sum = BoundExpr::Binary(OperatorKind::Add, Col(0), I(1), LogicalType::Integer());
    E gt = BoundExpr::Binary(OperatorKind::Gt, std::move(sum), I(5), LogicalType::Boolean());
    std::vector<E> like_args;
    like_args.push_back(Col(1, LogicalType::Varchar(), "s"));
    like_args.push_back(BoundExpr::Constant(Value::Varchar("a%")));
    E like = BoundExpr::Call(FunctionId::Like, std::move(like_args), LogicalType::Boolean());
    return BoundExpr::Binary(OperatorKind::And, std::move(gt), std::move(like),
                             LogicalType::Boolean());
}

} // namespace

TEST(BoundExpr, ToStringForms) {
    EXPECT_EQ(Sample()->ToString(), "(((#0 + 1) > 5) AND like(s, 'a%'))");
    EXPECT_EQ(Col(3, LogicalType::Integer(), "price")->ToString(), "price");
    EXPECT_EQ(BoundExpr::Constant(Value::Null(LogicalType::Integer()))->ToString(), "NULL");
    EXPECT_EQ(BoundExpr::Constant(Value::BigInt(7))->ToString(), "7::BIGINT");
    EXPECT_EQ(BoundExpr::Constant(Value::Varchar("it's"))->ToString(), "'it''s'");
    EXPECT_EQ(BoundExpr::Constant(Value::Date(Date::FromYMD(1998, 12, 1)))->ToString(),
              "DATE '1998-12-01'");
    EXPECT_EQ(BoundExpr::Cast(I(1), LogicalType::Double())->ToString(), "CAST(1 AS DOUBLE)");
    EXPECT_EQ(BoundExpr::Unary(OperatorKind::Negate, I(1), LogicalType::Integer())->ToString(),
              "(-1)");
    EXPECT_EQ(BoundExpr::Unary(OperatorKind::Not, BoundExpr::Constant(Value::Boolean(true)),
                               LogicalType::Boolean())
                  ->ToString(),
              "(NOT true)");
    EXPECT_EQ(BoundExpr::IsNull(Col(0), true)->ToString(), "(#0 IS NOT NULL)");
    std::vector<E> items;
    items.push_back(I(1));
    items.push_back(I(2));
    EXPECT_EQ(BoundExpr::InList(Col(0), std::move(items), false)->ToString(), "(#0 IN (1, 2))");
    std::vector<E> whens;
    whens.push_back(BoundExpr::Constant(Value::Boolean(true)));
    whens.push_back(I(1));
    whens.push_back(I(2));
    EXPECT_EQ(BoundExpr::Case(std::move(whens), LogicalType::Integer())->ToString(),
              "CASE WHEN true THEN 1 ELSE 2 END");
    std::vector<E> args;
    args.push_back(Col(0));
    EXPECT_EQ(BoundExpr::Aggregate(AggregateKind::Sum, std::move(args), true, LogicalType::BigInt())
                  ->ToString(),
              "sum(DISTINCT #0)");
    EXPECT_EQ(BoundExpr::Aggregate(AggregateKind::CountStar, {}, false, LogicalType::BigInt())
                  ->ToString(),
              "count(*)");
}

TEST(BoundExpr, CloneIsDeepAndEqual) {
    E a = Sample();
    E b = a->Clone();
    EXPECT_TRUE(a->Equals(*b));
    EXPECT_EQ(a->ToString(), b->ToString());
    // mutating the clone leaves the original untouched
    b->children[0]->children[1] = I(99);
    EXPECT_FALSE(a->Equals(*b));
    EXPECT_EQ(a->ToString(), "(((#0 + 1) > 5) AND like(s, 'a%'))");
    EXPECT_NE(b->ToString(), a->ToString());
}

TEST(BoundExpr, EqualsDistinguishesWhatMatters) {
    EXPECT_TRUE(Col(1, LogicalType::Integer(), "a")
                    ->Equals(*Col(1, LogicalType::Integer(), "other_name"))); // names ignored
    EXPECT_FALSE(Col(1)->Equals(*Col(2)));
    EXPECT_FALSE(Col(1, LogicalType::Integer())->Equals(*Col(1, LogicalType::BigInt())));
    EXPECT_TRUE(I(5)->Equals(*I(5)));
    EXPECT_FALSE(I(5)->Equals(*I(6)));
    EXPECT_FALSE(I(5)->Equals(*BoundExpr::Constant(Value::BigInt(5))));
    EXPECT_FALSE(BoundExpr::Constant(Value::Null(LogicalType::Integer()))->Equals(*I(0)));
    EXPECT_TRUE(BoundExpr::Constant(Value::Null(LogicalType::Integer()))
                    ->Equals(*BoundExpr::Constant(Value::Null(LogicalType::Integer()))));
    auto add = [](E l, E r) {
        return BoundExpr::Binary(OperatorKind::Add, std::move(l), std::move(r),
                                 LogicalType::Integer());
    };
    auto sub = [](E l, E r) {
        return BoundExpr::Binary(OperatorKind::Sub, std::move(l), std::move(r),
                                 LogicalType::Integer());
    };
    EXPECT_TRUE(add(Col(0), I(1))->Equals(*add(Col(0), I(1))));
    EXPECT_FALSE(add(Col(0), I(1))->Equals(*sub(Col(0), I(1))));
    EXPECT_FALSE(add(Col(0), I(1))->Equals(*add(I(1), Col(0)))); // operand order matters
    EXPECT_FALSE(BoundExpr::IsNull(Col(0), true)->Equals(*BoundExpr::IsNull(Col(0), false)));
    auto agg = [](AggregateKind k, bool distinct) {
        std::vector<E> a;
        a.push_back(Col(0));
        return BoundExpr::Aggregate(k, std::move(a), distinct, LogicalType::BigInt());
    };
    EXPECT_TRUE(agg(AggregateKind::Sum, false)->Equals(*agg(AggregateKind::Sum, false)));
    EXPECT_FALSE(agg(AggregateKind::Sum, false)->Equals(*agg(AggregateKind::Sum, true)));
    EXPECT_FALSE(agg(AggregateKind::Sum, false)->Equals(*agg(AggregateKind::Min, false)));
}

TEST(BoundExpr, TraversalPredicates) {
    E e = Sample();
    EXPECT_FALSE(e->IsConstantTree()); // has column references
    EXPECT_FALSE(e->ContainsAggregate());
    int nodes = 0;
    e->ForEach([&](const BoundExpr&) { nodes++; });
    EXPECT_EQ(nodes, 9); // AND, >, +, #0, 1, 5, like, s, 'a%'

    E constant = BoundExpr::Binary(OperatorKind::Add, I(1), I(2), LogicalType::Integer());
    EXPECT_TRUE(constant->IsConstantTree());
    std::vector<E> args;
    args.push_back(Col(0));
    E agg = BoundExpr::Aggregate(AggregateKind::Sum, std::move(args), false, LogicalType::BigInt());
    E wrapped = BoundExpr::Binary(OperatorKind::Add, std::move(agg), I(1), LogicalType::BigInt());
    EXPECT_TRUE(wrapped->ContainsAggregate());
    EXPECT_FALSE(wrapped->IsConstantTree());
}

TEST(BoundExprDeathTest, CaseNeedsAnElseAndPairs) {
    std::vector<E> bad;
    bad.push_back(I(1));
    bad.push_back(I(2)); // even length: no ELSE
    EXPECT_DEATH(BoundExpr::Case(std::move(bad), LogicalType::Integer()), "CDB_CHECK");
}

TEST(BoundExpr, NamesOfOperatorsFunctionsAndAggregates) {
    EXPECT_STREQ(OperatorText(OperatorKind::Concat), "||");
    EXPECT_STREQ(OperatorText(OperatorKind::Ne), "<>");
    EXPECT_STREQ(FunctionName(FunctionId::Substring), "substring");
    EXPECT_STREQ(AggregateName(AggregateKind::Avg), "avg");
}

} // namespace cdb
