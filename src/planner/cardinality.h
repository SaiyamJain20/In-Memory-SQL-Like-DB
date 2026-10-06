#pragma once

#include "planner/logical_plan.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace cdb {

// What the optimizer believes about one output column of an operator.
struct ColumnEstimate {
    double distinct = 1;      // distinct non-NULL values (>= 1)
    double null_fraction = 0; // of the operator's rows
    std::optional<Value> min; // bounds of the non-NULL values, when known
    std::optional<Value> max;
};

// How many rows an operator is expected to produce, and what its columns look like. Derived from
// the table statistics (storage/table_statistics.h) with the textbook rules: independence between
// predicates, uniform values between the bounds, containment of join keys (a foreign key's values
// are among the primary key's). The estimates only have to rank plans, so their errors matter
// where they change the ranking: the size of a join input relative to the other, not 5% here or
// there.
struct Estimate {
    double rows = 1;
    std::vector<ColumnEstimate> columns;
};

// A numeric image of an INTEGER / BIGINT / DOUBLE / DATE / BOOLEAN value (nullopt for the rest and
// for NaN), for interpolating inside bounds.
std::optional<double> NumericValue(const Value& v);

// `e` if it is a column reference, possibly under casts (a numeric widening does not change which
// column's statistics describe it); else null.
const BoundExpr* AsColumnRef(const BoundExpr& e);

// The fraction of the rows described by `input` for which `predicate` (over their columns) is TRUE.
double Selectivity(const BoundExpr& predicate, const Estimate& input);

// (Selectivity of an AND, with interval bounds on one column combined; see cardinality.cpp.)
double AndSelectivity(const BoundExpr& conjunction, const Estimate& input);

// `input` restricted to the rows that satisfy `predicate`: fewer rows, and the bounds and
// distinct counts of the columns it constrains narrowed.
Estimate ApplyFilter(const Estimate& input, const BoundExpr& predicate);

// The output of joining `left` and `right` on `condition` (over left ++ right; null for a cross
// join). A semi or anti join outputs the left columns only.
Estimate EstimateJoin(JoinType type, const Estimate& left, const Estimate& right,
                      const BoundExpr* condition);

// The estimates of every operator of a plan, computed bottom-up on demand and kept (the pointers
// into the plan must stay valid and the plan unchanged while an estimator is used).
class CardinalityEstimator {
  public:
    const Estimate& Of(const LogicalOperator& op);
    double Rows(const LogicalOperator& op) { return Of(op).rows; }

  private:
    Estimate Compute(const LogicalOperator& op);
    std::unordered_map<const LogicalOperator*, Estimate> memo_;
};

} // namespace cdb
