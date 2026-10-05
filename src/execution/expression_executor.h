#pragma once

#include "planner/bound_expression.h"
#include "vector/data_chunk.h"

#include <memory>

namespace cdb {

struct ExprNode;

// Evaluates one bound expression a whole vector (<= kVectorSize rows) at a time.
//
//  * Inputs may be in any vector format (flat / constant / dictionary): kernels read through
//    UnifiedFormat, so a filtered chunk (dictionary vectors over a selection) costs nothing extra.
//  * Kernels are typed and branch-light; the all-valid, flat case is a plain loop.
//  * AND / OR / CASE / COALESCE are lazy: the right-hand side is evaluated only for rows that can
//    still change the result, so run-time errors (overflow, bad casts) behave exactly as in the
//    row-at-a-time reference interpreter (EvaluateScalar), which the tests compare against.
//  * Predicates can be evaluated straight into a selection vector (Select), without
//    materialising a BOOLEAN vector, narrowing the selection between AND-ed conjuncts.
//
// An executor keeps per-node scratch vectors, so it is not thread-safe: use one per thread.
// The expression must outlive the executor.
class ExpressionExecutor {
  public:
    explicit ExpressionExecutor(const BoundExpr& expr);
    ~ExpressionExecutor();
    ExpressionExecutor(ExpressionExecutor&&) noexcept;
    ExpressionExecutor& operator=(ExpressionExecutor&&) noexcept;

    LogicalType type() const;

    // Evaluates the expression for every row of `input` into `result`, which must have the
    // expression's type and capacity >= input.size(). `result` is reset first. Throws
    // Error(Execution/Type) for run-time failures, like the scalar interpreter.
    void Execute(const DataChunk& input, Vector& result);

    // For a BOOLEAN expression: fills `selection` with the indices of the rows of `input` where
    // it is TRUE (NULL and FALSE rows are dropped), in increasing order, and returns their count.
    idx_t Select(const DataChunk& input, SelectionVector& selection);

  private:
    std::unique_ptr<ExprNode> root_;
};

} // namespace cdb
