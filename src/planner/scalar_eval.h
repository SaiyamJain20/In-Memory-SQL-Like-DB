#pragma once

#include "common/string_ops.h" // LikeMatch lives there (shared with the vectorized kernels)
#include "planner/bound_expression.h"

#include <span>

namespace cdb {

// Evaluates a bound expression for ONE row of input values (ColumnRef ordinal i reads row[i]).
//
// This is the engine's *reference semantics*: simple, obviously correct, row-at-a-time. It is used
// to run INSERT ... VALUES, to constant-fold, and - most importantly - as the oracle the
// vectorized kernels of Phase 4 are tested against. Semantics follow DuckDB:
//   * three-valued logic; AND/OR short-circuit on the left operand
//   * `/` is floating-point (1/0 = inf, 0.0/0 = nan); integer + - * and negation are
//     overflow-checked and throw Error(Execution)
//   * comparisons use Value::Compare's total order (so NaN = NaN and NaN sorts last)
// Aggregates cannot be evaluated here (they are not row functions).
Value EvaluateScalar(const BoundExpr& expr, std::span<const Value> row);

inline Value EvaluateConstant(const BoundExpr& expr) {
    return EvaluateScalar(expr, {});
}

} // namespace cdb
