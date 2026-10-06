#pragma once

#include "main/database.h"
#include "main/query_result.h"
#include "planner/cardinality.h"
#include "planner/logical_plan.h"

#include <string_view>

namespace cdb {

// A session on a Database: parses, binds and executes SQL. Query() never throws; failures come
// back as a QueryResult with an error code and a message that points at the offending SQL.
//
// SELECT and INSERT ... SELECT go through the optimizer (filter pushdown, join ordering, column
// pruning), the physical planner and the push-based pipeline executor. DDL, COPY and EXPLAIN are
// handled directly. EXPLAIN shows the optimized logical plan with estimated row counts; EXPLAIN
// ANALYZE runs the query and shows what each operator really produced and how long it took.
class Connection {
  public:
    explicit Connection(Database& db) : db_(db) {}

    // Runs every statement in `sql`; returns the result of the last one, stopping at the first
    // error.
    QueryResult Query(std::string_view sql);

    // Like Query(), but returns one result per statement executed (stopping after the first
    // failed one, whose result is the last element).
    std::vector<QueryResult> QueryAll(std::string_view sql);

    // Parses and binds one statement without executing it. Throws cdb::Error.
    LogicalPtr Plan(std::string_view sql);

    // Runs queries with or without the logical optimizer (on by default). With it off the plan
    // executes exactly as the binder produced it: the reference for optimizer tests and the "off"
    // arm of an ablation.
    void SetOptimizerEnabled(bool enabled) noexcept { optimize_ = enabled; }
    bool optimizer_enabled() const noexcept { return optimize_; }

  private:
    QueryResult Execute(LogicalPtr plan);
    QueryResult ExecuteSelect(LogicalPtr plan);
    QueryResult ExecuteInsert(LogicalPtr plan);
    // Runs the (optimized) query with a profile and returns the annotated plan.
    std::string ExplainAnalyzeQuery(const LogicalOperator& optimized,
                                    CardinalityEstimator& estimator);

    Database& db_;
    bool optimize_ = true;
};

} // namespace cdb
