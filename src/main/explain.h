#pragma once

#include "execution/pipeline.h"
#include "planner/cardinality.h"
#include "planner/logical_plan.h"

#include <string>

namespace cdb {

// EXPLAIN: the plan, one operator per line, each followed by the number of rows the optimizer
// expects it to produce: `JOIN INNER ON (a = b)  (~1000 rows)`.
std::string ExplainPlan(const LogicalOperator& root, CardinalityEstimator& estimator);

struct AnalyzeSummary {
    double planning_ms = 0;
    double execution_ms = 0;
    size_t threads = 1;
    uint64_t rows_returned = 0;
};

// EXPLAIN ANALYZE: the same tree after the query has run, with what each operator really did:
// `(est ~1000, actual 987 rows, 0.42 ms)`. A hash join also shows its build side, an aggregate,
// sort or guard the rows it consumed. Times are CPU time summed over the threads that ran the
// operator, so they can add up to more than the wall time printed at the end.
std::string ExplainAnalyze(const LogicalOperator& root, CardinalityEstimator& estimator,
                           const PhysicalPlan& plan, const ExecutionProfile& profile,
                           const AnalyzeSummary& summary);

} // namespace cdb
