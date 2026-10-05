#pragma once

#include "main/database.h"
#include "main/query_result.h"
#include "planner/logical_plan.h"

#include <string_view>

namespace cdb {

// A session on a Database: parses, binds and executes SQL. Query() never throws; failures come
// back as a QueryResult with an error code and a message that points at the offending SQL.
//
// Phase 3 executes DDL, INSERT ... VALUES, COPY ... FROM, EXPLAIN, and SELECTs that need no
// table (`SELECT 1 + 1`). Queries over tables are bound and plannable (see Plan / EXPLAIN) and
// reported as NotImplemented until the vectorized executor lands in Phase 4.
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

  private:
    QueryResult Execute(const LogicalOperator& plan);
    QueryResult ExecuteInsert(const LogicalInsert& insert);
    QueryResult ExecuteConstantSelect(const LogicalOperator& plan);

    Database& db_;
};

} // namespace cdb
