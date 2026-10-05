#pragma once

#include "common/types.h"
#include "types/value.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cdb {

// A bound (name-resolved, type-checked) expression. Unlike the parse tree, every node carries
// its result type, columns are referenced by ordinal into the operator's input row, and implicit
// casts are explicit nodes. One tagged struct (rather than a class hierarchy) keeps generic
// traversal, cloning and the optimizer's rewrites uniform.
enum class BoundKind : uint8_t {
    ColumnRef,
    Constant,
    Cast,
    Operator,
    Function,
    Case,
    InList,
    IsNull,
    Aggregate
};

enum class OperatorKind : uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Negate,
    Concat,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Not,
};

enum class FunctionId : uint8_t {
    Like,
    Year,
    Month,
    Day,
    DayOfWeek,
    DayOfYear,
    Quarter,
    Substring,
    Length,
    Upper,
    Lower,
    Abs,
    Round,
    Floor,
    Ceil,
    Coalesce,
    NullIf,
};

enum class AggregateKind : uint8_t { Count, CountStar, Sum, Avg, Min, Max };

const char* OperatorText(OperatorKind op) noexcept;
const char* FunctionName(FunctionId id) noexcept;
const char* AggregateName(AggregateKind kind) noexcept;

struct BoundExpr;
using BoundExprPtr = std::unique_ptr<BoundExpr>;

struct BoundExpr {
    BoundKind kind;
    LogicalType type;
    std::vector<BoundExprPtr> children;

    // ---- payload (which fields are meaningful depends on `kind`) -------------------------
    idx_t ordinal = 0;                              // ColumnRef: index into the input row
    std::string name;                               // ColumnRef: display name
    std::optional<Value> value;                     // Constant
    OperatorKind op = OperatorKind::Add;            // Operator
    FunctionId function = FunctionId::Like;         // Function
    AggregateKind aggregate = AggregateKind::Count; // Aggregate
    bool flag = false; // InList / IsNull: negated;  Aggregate: DISTINCT

    BoundExpr(BoundKind k, LogicalType t) : kind(k), type(t) {}
    BoundExpr(const BoundExpr&) = delete;
    BoundExpr& operator=(const BoundExpr&) = delete;

    // Factories ---------------------------------------------------------------------------
    static BoundExprPtr ColumnRef(idx_t ordinal, LogicalType type, std::string name = "");
    static BoundExprPtr Constant(Value value);
    static BoundExprPtr Cast(BoundExprPtr child, LogicalType target);
    static BoundExprPtr Unary(OperatorKind op, BoundExprPtr child, LogicalType type);
    static BoundExprPtr Binary(OperatorKind op, BoundExprPtr l, BoundExprPtr r, LogicalType type);
    static BoundExprPtr Call(FunctionId id, std::vector<BoundExprPtr> args, LogicalType type);
    // children = [when1, then1, ..., whenN, thenN, else]  (else is always present)
    static BoundExprPtr Case(std::vector<BoundExprPtr> whens_thens_else, LogicalType type);
    // children = [value, item1, item2, ...]
    static BoundExprPtr InList(BoundExprPtr value, std::vector<BoundExprPtr> items, bool negated);
    static BoundExprPtr IsNull(BoundExprPtr child, bool negated);
    static BoundExprPtr Aggregate(AggregateKind kind, std::vector<BoundExprPtr> args, bool distinct,
                                  LogicalType type);

    // Operations ---------------------------------------------------------------------------
    BoundExprPtr Clone() const;
    // Structural equality (kind, type, payload, children). Column display names are ignored.
    bool Equals(const BoundExpr& other) const;
    // Human-readable, fully parenthesised text (used by EXPLAIN and as a canonical key).
    std::string ToString() const;
    // True if the tree contains no column references or aggregates.
    bool IsConstantTree() const;
    bool ContainsAggregate() const;
    void ForEach(const std::function<void(const BoundExpr&)>& fn) const;
};

} // namespace cdb
