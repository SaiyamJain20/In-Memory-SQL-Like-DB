#pragma once

#include "types/logical_type.h"
#include "types/value.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cdb {

struct SelectStatement;

// ---------------------------------------------------------------------------------------------
// Expressions. The AST is purely syntactic: names are unresolved, types are unknown. Every node
// records the byte offset of its first token (`pos`) so the binder can point at the offending
// text. ToString() regenerates *fully parenthesised* SQL, which makes parse -> print -> parse a
// fixpoint (used heavily by the tests).
// ---------------------------------------------------------------------------------------------

enum class ExprKind : uint8_t {
    ColumnRef,
    Star,
    Constant,
    Interval,
    Unary,
    Binary,
    IsNull,
    Between,
    InList,
    InSubquery,
    Like,
    Case,
    Cast,
    Function,
    Extract,
    Exists,
    ScalarSubquery,
};

struct ParsedExpr {
    ExprKind kind;
    size_t pos;
    // Height of the expression tree rooted here (leaves are 1). The parser rejects trees deeper
    // than kMaxExprDepth so that recursive destruction / printing can never overflow the stack.
    uint32_t depth = 1;
    ParsedExpr(ExprKind k, size_t p) : kind(k), pos(p) {}
    virtual ~ParsedExpr() = default;
    ParsedExpr(const ParsedExpr&) = delete;
    ParsedExpr& operator=(const ParsedExpr&) = delete;
    virtual std::string ToString() const = 0;
};
using ExprPtr = std::unique_ptr<ParsedExpr>;

struct ColumnRefExpr : ParsedExpr {
    std::string table; // empty when unqualified
    std::string column;
    ColumnRefExpr(size_t p, std::string t, std::string c)
        : ParsedExpr(ExprKind::ColumnRef, p), table(std::move(t)), column(std::move(c)) {}
    std::string ToString() const override;
};

struct StarExpr : ParsedExpr {
    std::string table; // empty for a bare `*`
    StarExpr(size_t p, std::string t) : ParsedExpr(ExprKind::Star, p), table(std::move(t)) {}
    std::string ToString() const override;
};

enum class LiteralKind : uint8_t { Null, Boolean, Integer, Double, String, Date };

struct ConstantExpr : ParsedExpr {
    LiteralKind literal;
    Value value; // for Null: a NULL of an arbitrary placeholder type (the binder decides)
    ConstantExpr(size_t p, LiteralKind k, Value v)
        : ParsedExpr(ExprKind::Constant, p), literal(k), value(std::move(v)) {}
    std::string ToString() const override;
};

// INTERVAL '90' day, INTERVAL '3 month' ...
struct IntervalExpr : ParsedExpr {
    int64_t amount;
    std::string unit; // lower-case singular: year, month, week, day, hour, minute, second
    IntervalExpr(size_t p, int64_t a, std::string u)
        : ParsedExpr(ExprKind::Interval, p), amount(a), unit(std::move(u)) {}
    std::string ToString() const override;
};

enum class UnaryOp : uint8_t { Negate, Not };

struct UnaryExpr : ParsedExpr {
    UnaryOp op;
    ExprPtr child;
    UnaryExpr(size_t p, UnaryOp o, ExprPtr c)
        : ParsedExpr(ExprKind::Unary, p), op(o), child(std::move(c)) {}
    std::string ToString() const override;
};

enum class BinaryOp : uint8_t { Add, Sub, Mul, Div, Mod, Concat, Eq, Ne, Lt, Le, Gt, Ge, And, Or };

const char* BinaryOpText(BinaryOp op) noexcept;

struct BinaryExpr : ParsedExpr {
    BinaryOp op;
    ExprPtr left, right;
    BinaryExpr(size_t p, BinaryOp o, ExprPtr l, ExprPtr r)
        : ParsedExpr(ExprKind::Binary, p), op(o), left(std::move(l)), right(std::move(r)) {}
    std::string ToString() const override;
};

struct IsNullExpr : ParsedExpr {
    ExprPtr child;
    bool negated; // IS NOT NULL
    IsNullExpr(size_t p, ExprPtr c, bool n)
        : ParsedExpr(ExprKind::IsNull, p), child(std::move(c)), negated(n) {}
    std::string ToString() const override;
};

struct BetweenExpr : ParsedExpr {
    ExprPtr child, lower, upper;
    bool negated;
    BetweenExpr(size_t p, ExprPtr c, ExprPtr lo, ExprPtr hi, bool n)
        : ParsedExpr(ExprKind::Between, p), child(std::move(c)), lower(std::move(lo)),
          upper(std::move(hi)), negated(n) {}
    std::string ToString() const override;
};

struct InListExpr : ParsedExpr {
    ExprPtr child;
    std::vector<ExprPtr> list;
    bool negated;
    InListExpr(size_t p, ExprPtr c, std::vector<ExprPtr> l, bool n)
        : ParsedExpr(ExprKind::InList, p), child(std::move(c)), list(std::move(l)), negated(n) {}
    std::string ToString() const override;
};

struct InSubqueryExpr : ParsedExpr {
    ExprPtr child;
    std::shared_ptr<SelectStatement> subquery;
    bool negated;
    InSubqueryExpr(size_t p, ExprPtr c, std::shared_ptr<SelectStatement> s, bool n)
        : ParsedExpr(ExprKind::InSubquery, p), child(std::move(c)), subquery(std::move(s)),
          negated(n) {}
    std::string ToString() const override;
};

struct LikeExpr : ParsedExpr {
    ExprPtr child, pattern;
    bool negated;
    LikeExpr(size_t p, ExprPtr c, ExprPtr pat, bool n)
        : ParsedExpr(ExprKind::Like, p), child(std::move(c)), pattern(std::move(pat)), negated(n) {}
    std::string ToString() const override;
};

struct CaseExpr : ParsedExpr {
    struct When {
        ExprPtr condition;
        ExprPtr result;
    };
    ExprPtr operand; // null for the searched form `CASE WHEN cond ...`
    std::vector<When> whens;
    ExprPtr else_result; // may be null
    explicit CaseExpr(size_t p) : ParsedExpr(ExprKind::Case, p) {}
    std::string ToString() const override;
};

struct CastExpr : ParsedExpr {
    ExprPtr child;
    LogicalType target;
    CastExpr(size_t p, ExprPtr c, LogicalType t)
        : ParsedExpr(ExprKind::Cast, p), child(std::move(c)), target(t) {}
    std::string ToString() const override;
};

struct FunctionExpr : ParsedExpr {
    std::string name; // lower-case
    std::vector<ExprPtr> args;
    bool distinct = false; // count(DISTINCT x)
    bool star = false;     // count(*)
    FunctionExpr(size_t p, std::string n) : ParsedExpr(ExprKind::Function, p), name(std::move(n)) {}
    std::string ToString() const override;
};

struct ExtractExpr : ParsedExpr {
    std::string field; // lower-case: year, month, day, ...
    ExprPtr child;
    ExtractExpr(size_t p, std::string f, ExprPtr c)
        : ParsedExpr(ExprKind::Extract, p), field(std::move(f)), child(std::move(c)) {}
    std::string ToString() const override;
};

struct ExistsExpr : ParsedExpr {
    std::shared_ptr<SelectStatement> subquery;
    ExistsExpr(size_t p, std::shared_ptr<SelectStatement> s)
        : ParsedExpr(ExprKind::Exists, p), subquery(std::move(s)) {}
    std::string ToString() const override;
};

struct ScalarSubqueryExpr : ParsedExpr {
    std::shared_ptr<SelectStatement> subquery;
    ScalarSubqueryExpr(size_t p, std::shared_ptr<SelectStatement> s)
        : ParsedExpr(ExprKind::ScalarSubquery, p), subquery(std::move(s)) {}
    std::string ToString() const override;
};

// ---------------------------------------------------------------------------------------------
// FROM clause
// ---------------------------------------------------------------------------------------------

enum class TableRefKind : uint8_t { Base, Join, Subquery };

struct TableRef {
    TableRefKind kind;
    size_t pos;
    TableRef(TableRefKind k, size_t p) : kind(k), pos(p) {}
    virtual ~TableRef() = default;
    TableRef(const TableRef&) = delete;
    TableRef& operator=(const TableRef&) = delete;
    virtual std::string ToString() const = 0;
};
using TableRefPtr = std::unique_ptr<TableRef>;

struct BaseTableRef : TableRef {
    std::string name;
    std::string alias;                       // empty if none
    std::vector<std::string> column_aliases; // t AS x (a, b)
    BaseTableRef(size_t p, std::string n, std::string a)
        : TableRef(TableRefKind::Base, p), name(std::move(n)), alias(std::move(a)) {}
    std::string ToString() const override;
};

enum class JoinType : uint8_t { Inner, Left, Right, Full, Cross };

struct JoinRef : TableRef {
    JoinType type;
    TableRefPtr left, right;
    ExprPtr condition;                      // ON ...
    std::vector<std::string> using_columns; // USING (...)
    JoinRef(size_t p, JoinType t, TableRefPtr l, TableRefPtr r)
        : TableRef(TableRefKind::Join, p), type(t), left(std::move(l)), right(std::move(r)) {}
    std::string ToString() const override;
};

struct SubqueryRef : TableRef {
    std::shared_ptr<SelectStatement> select;
    std::string alias;
    std::vector<std::string> column_aliases; // (...) AS x (a, b)
    SubqueryRef(size_t p, std::shared_ptr<SelectStatement> s, std::string a)
        : TableRef(TableRefKind::Subquery, p), select(std::move(s)), alias(std::move(a)) {}
    std::string ToString() const override;
};

// ---------------------------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------------------------

enum class StatementKind : uint8_t {
    Select,
    CreateTable,
    DropTable,
    Insert,
    Copy,
    Explain,
    Checkpoint
};

struct Statement {
    StatementKind kind;
    explicit Statement(StatementKind k) : kind(k) {}
    virtual ~Statement() = default;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    virtual std::string ToString() const = 0;
};
using StatementPtr = std::unique_ptr<Statement>;

struct SelectItem {
    ExprPtr expr;
    std::string alias; // empty if none
};

enum class NullOrder : uint8_t { Default, First, Last };

struct OrderItem {
    ExprPtr expr;
    bool descending = false;
    NullOrder nulls = NullOrder::Default;
};

struct SelectStatement : Statement {
    bool distinct = false;
    std::vector<SelectItem> items;
    TableRefPtr from; // may be null: SELECT 1
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    ExprPtr having;
    std::vector<OrderItem> order_by;
    ExprPtr limit;
    ExprPtr offset;
    SelectStatement() : Statement(StatementKind::Select) {}
    std::string ToString() const override;
};

struct ColumnSpec {
    std::string name;
    LogicalType type;
    bool not_null = false;
};

struct CreateTableStatement : Statement {
    std::string name;
    std::vector<ColumnSpec> columns;
    bool if_not_exists = false;
    CreateTableStatement() : Statement(StatementKind::CreateTable) {}
    std::string ToString() const override;
};

struct DropTableStatement : Statement {
    std::string name;
    bool if_exists = false;
    DropTableStatement() : Statement(StatementKind::DropTable) {}
    std::string ToString() const override;
};

struct InsertStatement : Statement {
    std::string table;
    std::vector<std::string> columns;        // empty = all columns in table order
    std::vector<std::vector<ExprPtr>> rows;  // VALUES (...), (...)
    std::shared_ptr<SelectStatement> select; // or INSERT ... SELECT
    InsertStatement() : Statement(StatementKind::Insert) {}
    std::string ToString() const override;
};

struct CopyStatement : Statement {
    std::string table;
    std::string path;
    std::string delimiter = ",";
    bool header = false;
    CopyStatement() : Statement(StatementKind::Copy) {}
    std::string ToString() const override;
};

// CHECKPOINT: write a checkpoint now (a no-op for an in-memory database).
struct CheckpointStatement : Statement {
    CheckpointStatement() : Statement(StatementKind::Checkpoint) {}
    std::string ToString() const override;
};

struct ExplainStatement : Statement {
    bool analyze = false;
    StatementPtr inner;
    ExplainStatement() : Statement(StatementKind::Explain) {}
    std::string ToString() const override;
};

// Renders an identifier for SQL output: bare if it is a plain lower-case identifier that is not a
// reserved word, otherwise "double quoted" with embedded quotes doubled.
std::string QuoteIdentifier(const std::string& name);

// Renders a string as a SQL literal with embedded quotes doubled.
std::string QuoteString(const std::string& text);

} // namespace cdb
