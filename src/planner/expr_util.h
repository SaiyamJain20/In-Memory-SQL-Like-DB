#pragma once

#include "planner/bound_expression.h"

#include <functional>
#include <set>

namespace cdb {

// Helpers for rewriting bound expressions (used by the optimizer and the physical planner).

// The column ordinals the expression references.
void CollectColumnRefs(const BoundExpr& e, std::set<idx_t>& out);

// A copy of `e` with every column ordinal o replaced by map(o).
BoundExprPtr RemapColumns(const BoundExpr& e, const std::function<idx_t(idx_t)>& map);

// A copy of `e` in which every ColumnRef(i) is replaced by a copy of replacement[i].
BoundExprPtr SubstituteColumns(const BoundExpr& e, const std::vector<BoundExprPtr>& replacement);

// Flattens nested ANDs: (a AND b) AND c  ->  [a, b, c].
void SplitConjuncts(BoundExprPtr e, std::vector<BoundExprPtr>& out);

// a AND b AND ...; null for an empty list.
BoundExprPtr AndAll(std::vector<BoundExprPtr> conjuncts);

} // namespace cdb
