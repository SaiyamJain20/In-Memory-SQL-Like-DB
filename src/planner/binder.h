#pragma once

#include "catalog/catalog.h"
#include "parser/ast.h"
#include "planner/logical_plan.h"

#include <deque>

namespace cdb {

// Turns a parsed statement into a logical plan: resolves names against the catalog, infers and
// coerces types (inserting explicit casts), folds constants, and checks SQL's aggregation rules.
//
// Subqueries are unnested here, so the logical plan never contains one:
//   * [NOT] EXISTS (...) and [NOT] IN (SELECT ...) among the AND-ed conditions of a WHERE clause
//     become semi / anti joins (NOT IN: the null-aware kind), correlated or not;
//   * a scalar subquery (anywhere an expression may be) becomes a join: a cross join with its
//     single row when it is uncorrelated, a left join with it grouped by the correlation keys when
//     it is a correlated aggregate (`col > (SELECT avg(x) FROM t WHERE t.k = outer.k)`);
//   * WITH queries are inlined where they are referenced.
// Correlation must be equality between an inner and an outer expression (EXISTS may also carry any
// other condition mentioning both), through one level of nesting.
//
// Errors: Error(Binder) for unknown/ambiguous names and misuse of aggregates, Error(Type) for
// type mismatches and impossible casts, Error(Catalog) for missing tables, Error(NotImplemented)
// for valid SQL not supported yet (other subquery shapes, non-constant intervals). Errors that
// originate in an expression carry the byte position of that expression in the SQL text.
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
    // The scalar subqueries met while binding one query block's expressions. Each is recorded as
    // `Pending` and stands in the expression as a SubqueryValue leaf; the block joins them onto its
    // plan (AttachScalars) and replaces the leaves with column references.
    struct SubqueryCollector {
        struct Pending {
            LogicalPtr plan;                      // uncorrelated: exactly one row, one column
            bool correlated = false;              // else: [keys..., value] grouped by the keys
            std::vector<BoundExprPtr> outer_keys; // over the enclosing block's input row
            BoundExprPtr empty_default;           // the value for an outer row with no group
            LogicalType type = LogicalType::Integer();
            BoundExprPtr replacement; // set when attached
            size_t pos = 0;           // where the subquery is, for errors
        };
        std::vector<Pending> pending;
    };
    struct ExprContext {
        const Scope* scope = nullptr;
        bool allow_aggregates = false;
        const ExprContext* outer = nullptr;     // the enclosing query block, for correlated names
        SubqueryCollector* collector = nullptr; // null: scalar subqueries are not allowed here
    };
    // A query block's FROM and WHERE, bound: the plan (with the conditions that do not mention the
    // enclosing block applied), its scope, and the conditions that do (`correlated`, with
    // OuterColumn leaves) when the caller asked for them.
    struct Block {
        LogicalPtr plan;
        Scope scope;
        std::vector<BoundExprPtr> correlated;
    };
    // WITH entries visible while binding: each knows the entries visible to its own body.
    struct CteEntry {
        std::string name;
        const CommonTableExpression* definition;
        const CteEntry* visible_to_body;
    };
    // Makes the WITH queries of a SELECT visible while it is bound; the entries are removed again
    // when the object goes out of scope.
    class CteScope {
      public:
        CteScope(Binder& binder, const SelectStatement& select);
        ~CteScope();
        CteScope(const CteScope&) = delete;
        CteScope& operator=(const CteScope&) = delete;

      private:
        void Unwind();

        Binder& binder_;
        const CteEntry* saved_head_;
        size_t count_;
    };
    enum class Lookup { Found, NotFound, Ambiguous };

    // ---- statements (binder.cpp) ----------------------------------------------------------
    LogicalPtr BindSelect(const SelectStatement& select, const ExprContext* outer = nullptr);
    Block BindFromWhere(const SelectStatement& select, const ExprContext* outer,
                        bool allow_correlated, SubqueryCollector& collector);
    LogicalPtr BindSelectBody(const SelectStatement& select, const ExprContext* outer, Block block,
                              SubqueryCollector& collector);
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
    // The column reference as a bound leaf, or null if no enclosing block has such a column.
    BoundExprPtr TryBindColumnRef(const ColumnRefExpr& e, const ExprContext& ctx);
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

    // ---- subqueries (bind_subquery.cpp) -----------------------------------------------------
    BoundExprPtr BindScalarSubquery(const ScalarSubqueryExpr& e, const ExprContext& ctx);
    bool IsSubqueryPredicate(const ParsedExpr& e) const;
    // Joins the conjunct `pred` ([NOT] EXISTS / [NOT] IN subquery) onto `plan`.
    void ApplySubqueryPredicate(LogicalPtr& plan, Scope& scope, const ParsedExpr& pred,
                                const ExprContext& ctx);
    void ApplyExists(LogicalPtr& plan, const SelectStatement& sub, bool negated,
                     const ExprContext& ctx, size_t pos);
    void ApplyIn(LogicalPtr& plan, const InSubqueryExpr& in, bool negated, const ExprContext& ctx);
    // Joins the scalar subqueries recorded in `collector` since the last call onto `plan` and
    // records, for each, the expression that replaces its SubqueryValue leaf. Correlated ones need
    // the pre-aggregation input row (`allow_correlated`).
    void AttachScalars(LogicalPtr& plan, Scope* scope, SubqueryCollector& collector,
                       bool allow_correlated);
    void ReplaceSubqueryValues(BoundExprPtr& e, const SubqueryCollector& collector) const;
    const CteEntry* FindCte(const std::string& name) const;
    BoundTable BindCte(const CteEntry& cte, const BaseTableRef& ref);

    Catalog& catalog_;
    std::deque<CteEntry> cte_storage_;
    const CteEntry* cte_head_ = nullptr;
};

} // namespace cdb
