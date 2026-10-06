#pragma once

#include "parser/ast.h"
#include "planner/bound_expression.h"
#include "storage/table.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cdb {

// The logical plan: a tree of relational operators over bound expressions. Every operator knows
// the names and types of the columns it outputs; expressions refer to their input by ordinal
// (a join's input is the left child's columns followed by the right child's).
enum class LogicalKind : uint8_t {
    Get,
    Filter,
    Projection,
    Aggregate,
    Join,
    Order,
    Limit,
    Distinct,
    Values,
    CreateTable,
    DropTable,
    Insert,
    Copy,
    Explain,
    Checkpoint,
    ScalarGuard,
};

struct LogicalOperator;
using LogicalPtr = std::unique_ptr<LogicalOperator>;

struct LogicalOperator {
    LogicalKind kind;
    std::vector<LogicalPtr> children;
    std::vector<std::string> names; // output column names
    std::vector<LogicalType> types; // output column types

    explicit LogicalOperator(LogicalKind k) : kind(k) {}
    virtual ~LogicalOperator() = default;
    LogicalOperator(const LogicalOperator&) = delete;
    LogicalOperator& operator=(const LogicalOperator&) = delete;

    // One-line description of this operator (without its children).
    virtual std::string Describe() const = 0;
    // The whole subtree, one operator per line, children indented by two spaces.
    std::string ToString(int indent = 0) const;
    idx_t ColumnCount() const { return types.size(); }
};

struct LogicalGet : LogicalOperator {
    std::shared_ptr<Table> table;
    std::vector<idx_t> column_ids;    // table columns produced, in output order
    std::vector<TableFilter> filters; // zone-map pruning hints (set by the optimizer)
    LogicalGet() : LogicalOperator(LogicalKind::Get) {}
    std::string Describe() const override;
};

struct LogicalFilter : LogicalOperator {
    BoundExprPtr predicate;
    LogicalFilter() : LogicalOperator(LogicalKind::Filter) {}
    std::string Describe() const override;
};

struct LogicalProjection : LogicalOperator {
    std::vector<BoundExprPtr> exprs;
    LogicalProjection() : LogicalOperator(LogicalKind::Projection) {}
    std::string Describe() const override;
};

// Output: the group expressions, then the aggregates (ordinals 0..G-1, then G..G+A-1).
struct LogicalAggregate : LogicalOperator {
    std::vector<BoundExprPtr> groups;
    std::vector<BoundExprPtr> aggregates;
    LogicalAggregate() : LogicalOperator(LogicalKind::Aggregate) {}
    std::string Describe() const override;
};

// Semi / Anti / AntiNullAware joins (from unnested subqueries) output the LEFT columns only; their
// condition is still over left ++ right.
struct LogicalJoin : LogicalOperator {
    JoinType join_type = JoinType::Inner;
    BoundExprPtr condition; // null for CROSS joins; over left ++ right columns otherwise
    LogicalJoin() : LogicalOperator(LogicalKind::Join) {}
    std::string Describe() const override;
};

struct SortKey {
    BoundExprPtr expr;
    bool descending = false;
    bool nulls_first = false; // default (like DuckDB): NULLS LAST for both directions
};

struct LogicalOrder : LogicalOperator {
    std::vector<SortKey> keys;
    LogicalOrder() : LogicalOperator(LogicalKind::Order) {}
    std::string Describe() const override;
};

struct LogicalLimit : LogicalOperator {
    std::optional<int64_t> limit; // nullopt = unlimited
    int64_t offset = 0;
    LogicalLimit() : LogicalOperator(LogicalKind::Limit) {}
    std::string Describe() const override;
};

struct LogicalDistinct : LogicalOperator {
    LogicalDistinct() : LogicalOperator(LogicalKind::Distinct) {}
    std::string Describe() const override;
};

// Literal rows. `SELECT 1` is a Projection over a Values with one row and no columns.
struct LogicalValues : LogicalOperator {
    std::vector<std::vector<BoundExprPtr>> rows;
    LogicalValues() : LogicalOperator(LogicalKind::Values) {}
    std::string Describe() const override;
};

struct LogicalCreateTable : LogicalOperator {
    std::string table_name;
    std::vector<ColumnDefinition> schema;
    bool if_not_exists = false;
    LogicalCreateTable() : LogicalOperator(LogicalKind::CreateTable) {}
    std::string Describe() const override;
};

struct LogicalDropTable : LogicalOperator {
    std::string table_name;
    bool if_exists = false;
    LogicalDropTable() : LogicalOperator(LogicalKind::DropTable) {}
    std::string Describe() const override;
};

// Child output already has the target table's column count and types, in table order.
struct LogicalInsert : LogicalOperator {
    std::shared_ptr<Table> table;
    LogicalInsert() : LogicalOperator(LogicalKind::Insert) {}
    std::string Describe() const override;
};

struct LogicalCopy : LogicalOperator {
    std::shared_ptr<Table> table;
    std::string path;
    std::string delimiter = ",";
    bool header = false;
    LogicalCopy() : LogicalOperator(LogicalKind::Copy) {}
    std::string Describe() const override;
};

// Exactly one row: the child's row; a row of NULLs if the child has none; an error if it has
// several. What a scalar subquery that is not statically a single row is wrapped in.
struct LogicalScalarGuard : LogicalOperator {
    LogicalScalarGuard() : LogicalOperator(LogicalKind::ScalarGuard) {}
    std::string Describe() const override;
};

struct LogicalCheckpoint : LogicalOperator {
    LogicalCheckpoint() : LogicalOperator(LogicalKind::Checkpoint) {}
    std::string Describe() const override;
};

struct LogicalExplain : LogicalOperator {
    bool analyze = false;
    LogicalExplain() : LogicalOperator(LogicalKind::Explain) {}
    std::string Describe() const override;
};

} // namespace cdb
