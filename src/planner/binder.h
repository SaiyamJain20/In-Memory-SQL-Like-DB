#pragma once

#include "catalog/catalog.h"
#include "parser/ast.h"
#include "planner/logical_plan.h"

namespace cdb {

// Turns a parsed statement into a logical plan: resolves names against the catalog, infers and
// coerces types (inserting explicit casts), folds constants, and checks SQL's aggregation rules.
//
// Errors: Error(Binder) for unknown/ambiguous names and misuse of aggregates, Error(Type) for
// type mismatches and impossible casts, Error(Catalog) for missing tables, Error(NotImplemented)
// for valid SQL not supported yet (subqueries, non-constant intervals). Errors that originate in
// an expression carry the byte position of that expression in the SQL text.
class Binder {
  public:
    explicit Binder(Catalog& catalog) : catalog_(catalog) {}

    LogicalPtr Bind(const Statement& statement);

  private:
    // ---- scopes ---------------------------------------------------------------------------
    struct ScopeColumn {
        std::string table; // alias the column is reachable through ("" for none)
        std::string name;
        LogicalType type;
        bool hidden = false; // dropped from `*` and unqualified lookup (USING duplicates)
    };
    struct Scope {
        std::vector<ScopeColumn> columns;
    };
    struct BoundTable {
        LogicalPtr plan;
        Scope scope; // aligned with plan->names / plan->types
    };
    struct ExprContext {
        const Scope* scope = nullptr;
        bool allow_aggregates = false;
    };
    enum class Lookup { Found, NotFound, Ambiguous };

    // ---- statements (binder.cpp) ----------------------------------------------------------
    LogicalPtr BindSelect(const SelectStatement& select);
    LogicalPtr BindCreateTable(const CreateTableStatement& stmt);
    LogicalPtr BindDropTable(const DropTableStatement& stmt);
    LogicalPtr BindInsert(const InsertStatement& stmt);
    LogicalPtr BindCopy(const CopyStatement& stmt);
    LogicalPtr BindExplain(const ExplainStatement& stmt);

    BoundTable BindTableRef(const TableRef& ref);
    BoundTable BindBaseTable(const BaseTableRef& ref);
    BoundTable BindJoin(const JoinRef& ref);
    BoundTable BindSubqueryRef(const SubqueryRef& ref);

    // ---- expressions (bind_expression.cpp) ------------------------------------------------
    BoundExprPtr BindExpr(const ParsedExpr& e, const ExprContext& ctx);
    BoundExprPtr BindColumnRef(const ColumnRefExpr& e, const ExprContext& ctx);
    BoundExprPtr BindConstant(const ConstantExpr& e);
    BoundExprPtr BindUnary(const UnaryExpr& e, const ExprContext& ctx);
    BoundExprPtr BindBinary(const BinaryExpr& e, const ExprContext& ctx);
    BoundExprPtr BindDateInterval(const BinaryExpr& e, const ExprContext& ctx);
    BoundExprPtr BindBetween(const BetweenExpr& e, const ExprContext& ctx);
    BoundExprPtr BindInList(const InListExpr& e, const ExprContext& ctx);
    BoundExprPtr BindLike(const LikeExpr& e, const ExprContext& ctx);
    BoundExprPtr BindCase(const CaseExpr& e, const ExprContext& ctx);
    BoundExprPtr BindCast(const CastExpr& e, const ExprContext& ctx);
    BoundExprPtr BindFunction(const FunctionExpr& e, const ExprContext& ctx);
    BoundExprPtr BindAggregate(const FunctionExpr& e, const ExprContext& ctx);
    BoundExprPtr BindScalarFunction(const std::string& name, std::vector<BoundExprPtr> args,
                                    size_t pos);
    BoundExprPtr BindExtract(const ExtractExpr& e, const ExprContext& ctx);

    static Lookup ResolveColumn(const Scope& scope, const std::string& table,
                                const std::string& column, idx_t* ordinal);

    Catalog& catalog_;
};

} // namespace cdb
