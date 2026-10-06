// Subqueries: WITH, [NOT] EXISTS, [NOT] IN (SELECT ...) and scalar subqueries, unnested into joins
// while binding (see the comment on Binder).

#include "common/error.h"
#include "planner/binder.h"
#include "planner/expr_util.h"
#include "planner/scalar_eval.h"
#include "types/cast.h"

#include <algorithm>
#include <cctype>
#include <functional>

namespace cdb {

namespace {

std::string Lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

[[noreturn]] void Fail(ErrorCode code, const std::string& message, size_t pos) {
    throw Error(code, message, pos);
}

// a = b over operands that may differ in type: both sides are cast to their common type.
BoundExprPtr MakeEquality(BoundExprPtr l, BoundExprPtr r, size_t pos) {
    if (l->type != r->type) {
        const auto common = CommonSuperType(l->type, r->type);
        if (!common) {
            Fail(ErrorCode::Type,
                 "cannot compare " + l->type.ToString() + " with " + r->type.ToString(), pos);
        }
        if (l->type != *common) {
            l = BoundExpr::Cast(std::move(l), *common);
        }
        if (r->type != *common) {
            r = BoundExpr::Cast(std::move(r), *common);
        }
    }
    return BoundExpr::Binary(OperatorKind::Eq, std::move(l), std::move(r), LogicalType::Boolean());
}

bool HasColumnRef(const BoundExpr& e) {
    bool found = false;
    e.ForEach([&](const BoundExpr& x) { found = found || x.kind == BoundKind::ColumnRef; });
    return found;
}

// A copy in which OuterColumn(o) is ColumnRef(o): `e` is then over the enclosing block's input row.
BoundExprPtr OuterToColumnRef(const BoundExpr& e) {
    BoundExprPtr copy = e.Clone();
    std::function<void(BoundExpr&)> walk = [&](BoundExpr& x) {
        if (x.kind == BoundKind::OuterColumn) {
            x.kind = BoundKind::ColumnRef;
        }
        for (auto& c : x.children) {
            walk(*c);
        }
    };
    walk(*copy);
    return copy;
}

// A condition between the enclosing block's row (`outer`, columns [0, lw)) and a subquery's row
// (columns [lw, ...)): the subquery's own columns shift by lw, outer columns keep their ordinals.
BoundExprPtr ToJoinCondition(const BoundExpr& e, idx_t lw) {
    BoundExprPtr copy = e.Clone();
    std::function<void(BoundExpr&)> walk = [&](BoundExpr& x) {
        if (x.kind == BoundKind::ColumnRef) {
            x.ordinal += lw;
        } else if (x.kind == BoundKind::OuterColumn) {
            x.kind = BoundKind::ColumnRef;
        }
        for (auto& c : x.children) {
            walk(*c);
        }
    };
    walk(*copy);
    return copy;
}

struct Correlation {
    // inner_expr (over the subquery's row) = outer_expr (over the enclosing row, as ColumnRefs)
    std::vector<std::pair<BoundExprPtr, BoundExprPtr>> keys;
    std::vector<BoundExprPtr> residual; // everything else that mentions both (OuterColumn leaves)
};

// Splits the correlated conditions of a subquery into equality keys and the rest.
Correlation SplitCorrelation(std::vector<BoundExprPtr>& conditions) {
    Correlation out;
    for (BoundExprPtr& c : conditions) {
        if (c->kind == BoundKind::Operator && c->op == OperatorKind::Eq) {
            BoundExpr& a = *c->children[0];
            BoundExpr& b = *c->children[1];
            const bool a_outer = a.ContainsOuterColumn(), b_outer = b.ContainsOuterColumn();
            if (!a_outer && b_outer && HasColumnRef(a) && !HasColumnRef(b)) {
                out.keys.emplace_back(std::move(c->children[0]), OuterToColumnRef(b));
                continue;
            }
            if (a_outer && !b_outer && HasColumnRef(b) && !HasColumnRef(a)) {
                out.keys.emplace_back(std::move(c->children[1]), OuterToColumnRef(a));
                continue;
            }
        }
        out.residual.push_back(std::move(c));
    }
    conditions.clear();
    return out;
}

// Does any expression of the plan mention a column of an enclosing block?
bool PlanContainsOuter(const LogicalOperator& op) {
    const auto has = [](const BoundExprPtr& e) { return e && e->ContainsOuterColumn(); };
    switch (op.kind) {
    case LogicalKind::Filter:
        if (has(static_cast<const LogicalFilter&>(op).predicate))
            return true;
        break;
    case LogicalKind::Projection:
        for (const auto& e : static_cast<const LogicalProjection&>(op).exprs)
            if (has(e))
                return true;
        break;
    case LogicalKind::Aggregate: {
        const auto& a = static_cast<const LogicalAggregate&>(op);
        for (const auto& e : a.groups)
            if (has(e))
                return true;
        for (const auto& e : a.aggregates)
            if (has(e))
                return true;
        break;
    }
    case LogicalKind::Join:
        if (has(static_cast<const LogicalJoin&>(op).condition))
            return true;
        break;
    case LogicalKind::Order:
        for (const auto& k : static_cast<const LogicalOrder&>(op).keys)
            if (has(k.expr))
                return true;
        break;
    case LogicalKind::Values:
        for (const auto& row : static_cast<const LogicalValues&>(op).rows)
            for (const auto& e : row)
                if (has(e))
                    return true;
        break;
    default:
        break;
    }
    for (const auto& child : op.children) {
        if (PlanContainsOuter(*child)) {
            return true;
        }
    }
    return false;
}

// A projection over an ungrouped aggregate always produces exactly one row.
bool IsSingleRow(const LogicalOperator& op) {
    return op.kind == LogicalKind::Projection && op.children[0]->kind == LogicalKind::Aggregate &&
           static_cast<const LogicalAggregate&>(*op.children[0]).groups.empty();
}

// The value of a post-aggregate expression over an empty group: count(...) is 0, every other
// aggregate NULL. Null if that cannot be folded or is NULL.
BoundExprPtr EmptyGroupValue(const BoundExpr& post, idx_t first_aggregate_column,
                             const std::vector<BoundExprPtr>& aggregates) {
    BoundExprPtr copy = post.Clone();
    std::function<bool(BoundExprPtr&)> subst = [&](BoundExprPtr& x) -> bool {
        if (x->kind == BoundKind::ColumnRef) {
            const idx_t a = x->ordinal - first_aggregate_column;
            const BoundExpr& agg = *aggregates.at(a);
            if (agg.aggregate == AggregateKind::Count ||
                agg.aggregate == AggregateKind::CountStar) {
                x = BoundExpr::Constant(Value::BigInt(0));
            } else {
                x = BoundExpr::Constant(Value::Null(x->type));
            }
            return true;
        }
        for (auto& c : x->children) {
            subst(c);
        }
        return true;
    };
    subst(copy);
    try {
        const Value v = EvaluateConstant(*copy);
        if (v.IsNull()) {
            return nullptr;
        }
        return BoundExpr::Constant(v);
    } catch (const Error&) {
        return nullptr;
    }
}

} // namespace

// ---------------------------------------------------------------------------------- WITH

const Binder::CteEntry* Binder::FindCte(const std::string& name) const {
    for (const CteEntry* e = cte_head_; e != nullptr; e = e->visible_to_body) {
        if (e->name == name) {
            return e;
        }
    }
    return nullptr;
}

Binder::BoundTable Binder::BindCte(const CteEntry& cte, const BaseTableRef& ref) {
    // the body sees the entries that were visible where it was defined: it cannot refer to itself
    const CteEntry* saved = cte_head_;
    cte_head_ = cte.visible_to_body;
    LogicalPtr plan;
    try {
        plan = BindSelect(*cte.definition->select);
    } catch (...) {
        cte_head_ = saved;
        throw;
    }
    cte_head_ = saved;
    const auto& declared = cte.definition->columns;
    if (declared.size() > plan->ColumnCount() || ref.column_aliases.size() > plan->ColumnCount()) {
        Fail(ErrorCode::Binder,
             "WITH query \"" + cte.name + "\" has " + std::to_string(plan->ColumnCount()) +
                 " columns but more column names were given",
             ref.pos);
    }
    BoundTable out;
    const std::string alias = Lower(ref.alias.empty() ? cte.name : ref.alias);
    for (idx_t i = 0; i < plan->ColumnCount(); i++) {
        if (i < declared.size()) {
            plan->names[i] = Lower(declared[i]);
        }
        if (i < ref.column_aliases.size()) {
            plan->names[i] = Lower(ref.column_aliases[i]);
        }
        out.scope.columns.push_back({alias, Lower(plan->names[i]), plan->types[i], false});
    }
    out.plan = std::move(plan);
    return out;
}

// ---------------------------------------------------------------------------------- scalars

void Binder::ReplaceSubqueryValues(BoundExprPtr& e, const SubqueryCollector& collector) const {
    if (e->kind == BoundKind::SubqueryValue) {
        const auto& p = collector.pending.at(e->ordinal);
        CDB_CHECK(p.replacement != nullptr);
        e = p.replacement->Clone();
        return;
    }
    for (auto& c : e->children) {
        ReplaceSubqueryValues(c, collector);
    }
}

void Binder::AttachScalars(LogicalPtr& plan, Scope* scope, SubqueryCollector& collector,
                           bool allow_correlated) {
    for (size_t i = 0; i < collector.pending.size(); i++) {
        SubqueryCollector::Pending& p = collector.pending[i];
        if (p.replacement != nullptr) {
            continue;
        }
        const idx_t lw = plan->ColumnCount();
        const std::string base = "$scalar" + std::to_string(i);
        auto join = std::make_unique<LogicalJoin>();
        join->names = plan->names;
        join->types = plan->types;
        if (!p.correlated) {
            join->join_type = JoinType::Cross;
            p.replacement = BoundExpr::ColumnRef(lw, p.type, base);
        } else {
            if (!allow_correlated || scope == nullptr) {
                Fail(ErrorCode::NotImplemented,
                     "a correlated subquery is only supported in the WHERE clause, or in the "
                     "select list of a query without aggregation",
                     p.pos);
            }
            join->join_type = JoinType::Left;
            std::vector<BoundExprPtr> conditions;
            const idx_t keys = p.plan->ColumnCount() - 1;
            for (idx_t k = 0; k < keys; k++) {
                conditions.push_back(MakeEquality(
                    p.outer_keys[k]->Clone(),
                    BoundExpr::ColumnRef(lw + k, p.plan->types[k], base + "_key"), p.pos));
            }
            join->condition = AndAll(std::move(conditions));
            BoundExprPtr value = BoundExpr::ColumnRef(lw + keys, p.type, base);
            if (p.empty_default != nullptr) {
                // an outer row with no group gets the aggregate's value over no rows (count: 0)
                std::vector<BoundExprPtr> branches;
                branches.push_back(BoundExpr::IsNull(
                    BoundExpr::ColumnRef(lw, p.plan->types[0], base + "_key"), /*negated=*/true));
                branches.push_back(std::move(value));
                branches.push_back(p.empty_default->Clone());
                p.replacement = BoundExpr::Case(std::move(branches), p.type);
            } else {
                p.replacement = std::move(value);
            }
        }
        for (idx_t c = 0; c < p.plan->ColumnCount(); c++) {
            join->names.push_back(p.plan->names[c]);
            join->types.push_back(p.plan->types[c]);
            if (scope != nullptr) {
                scope->columns.push_back(
                    {"", base + "_" + std::to_string(c), p.plan->types[c], true});
            }
        }
        join->children.push_back(std::move(plan));
        join->children.push_back(std::move(p.plan));
        plan = std::move(join);
    }
}

BoundExprPtr Binder::BindScalarSubquery(const ScalarSubqueryExpr& e, const ExprContext& ctx) {
    if (ctx.collector == nullptr) {
        Fail(ErrorCode::NotImplemented, "a subquery is not supported in this position", e.pos);
    }
    const SelectStatement& sub = *e.subquery;
    CteScope ctes(*this, sub);
    SubqueryCollector inner;
    Block block = BindFromWhere(sub, &ctx, /*allow_correlated=*/true, inner);
    SubqueryCollector::Pending pending;

    if (block.correlated.empty()) {
        // uncorrelated: the subquery runs once and its single value joins every row
        LogicalPtr plan = BindSelectBody(sub, &ctx, std::move(block), inner);
        if (PlanContainsOuter(*plan)) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated reference outside the WHERE clause of a subquery is not supported "
                 "yet",
                 e.pos);
        }
        if (plan->ColumnCount() != 1) {
            Fail(ErrorCode::Binder,
                 "a subquery used as an expression must return one column, not " +
                     std::to_string(plan->ColumnCount()),
                 e.pos);
        }
        pending.type = plan->types[0];
        if (IsSingleRow(*plan)) {
            pending.plan = std::move(plan);
        } else {
            auto guard = std::make_unique<LogicalScalarGuard>();
            guard->names = plan->names;
            guard->types = plan->types;
            guard->children.push_back(std::move(plan));
            pending.plan = std::move(guard);
        }
    } else {
        // correlated: only `SELECT <aggregate expression> FROM ... WHERE inner = outer AND ...`
        if (sub.items.size() != 1 || sub.items[0].expr->kind == ExprKind::Star ||
            !sub.group_by.empty() || sub.having || sub.distinct || !sub.order_by.empty() ||
            sub.limit || sub.offset) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated scalar subquery must be a single aggregate expression without "
                 "GROUP BY, HAVING, DISTINCT, ORDER BY or LIMIT",
                 e.pos);
        }
        const ExprContext inner_ctx{&block.scope, true, &ctx, &inner};
        BoundExprPtr value = BindExpr(*sub.items[0].expr, inner_ctx);
        if (!value->ContainsAggregate()) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated scalar subquery must be an aggregate (e.g. min, max, sum, avg, "
                 "count)",
                 e.pos);
        }
        if (value->ContainsOuterColumn() || value->ContainsSubqueryValue()) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated scalar subquery cannot mention outer columns or other subqueries "
                 "in its select list",
                 e.pos);
        }
        Correlation corr = SplitCorrelation(block.correlated);
        if (!corr.residual.empty()) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated scalar subquery supports only equality correlation (inner = outer)",
                 e.pos);
        }
        const idx_t keys = corr.keys.size();
        std::vector<BoundExprPtr> aggregates;
        std::function<BoundExprPtr(BoundExprPtr)> lift = [&](BoundExprPtr x) -> BoundExprPtr {
            if (x->kind == BoundKind::Aggregate) {
                for (const auto& arg : x->children) {
                    if (arg->ContainsAggregate()) {
                        Fail(ErrorCode::Binder, "aggregate function calls cannot be nested", e.pos);
                    }
                }
                for (idx_t a = 0; a < aggregates.size(); a++) {
                    if (x->Equals(*aggregates[a])) {
                        return BoundExpr::ColumnRef(keys + a, x->type, aggregates[a]->ToString());
                    }
                }
                aggregates.push_back(x->Clone());
                return BoundExpr::ColumnRef(keys + aggregates.size() - 1, x->type, x->ToString());
            }
            if (x->kind == BoundKind::ColumnRef) {
                Fail(ErrorCode::Binder,
                     "column \"" + x->name +
                         "\" must be used in an aggregate function in this subquery",
                     e.pos);
            }
            for (auto& c : x->children) {
                c = lift(std::move(c));
            }
            return x;
        };
        BoundExprPtr post = lift(std::move(value));

        auto agg = std::make_unique<LogicalAggregate>();
        auto proj = std::make_unique<LogicalProjection>();
        for (idx_t k = 0; k < keys; k++) {
            agg->names.push_back("$key" + std::to_string(k));
            agg->types.push_back(corr.keys[k].first->type);
            proj->names.push_back("$key" + std::to_string(k));
            proj->types.push_back(corr.keys[k].first->type);
            proj->exprs.push_back(BoundExpr::ColumnRef(k, corr.keys[k].first->type));
            agg->groups.push_back(std::move(corr.keys[k].first));
            pending.outer_keys.push_back(std::move(corr.keys[k].second));
        }
        for (auto& a : aggregates) {
            agg->names.push_back(a->ToString());
            agg->types.push_back(a->type);
        }
        pending.empty_default = EmptyGroupValue(*post, keys, aggregates);
        pending.type = post->type;
        agg->aggregates = std::move(aggregates);
        agg->children.push_back(std::move(block.plan));
        proj->names.push_back("$value");
        proj->types.push_back(post->type);
        proj->exprs.push_back(std::move(post));
        proj->children.push_back(std::move(agg));
        pending.plan = std::move(proj);
        pending.correlated = true;
    }
    const idx_t index = ctx.collector->pending.size();
    pending.pos = e.pos;
    const LogicalType type = pending.type;
    ctx.collector->pending.push_back(std::move(pending));
    return BoundExpr::SubqueryValue(index, type);
}

// ---------------------------------------------------------------------------------- predicates

bool Binder::IsSubqueryPredicate(const ParsedExpr& e) const {
    if (e.kind == ExprKind::Exists || e.kind == ExprKind::InSubquery) {
        return true;
    }
    if (e.kind == ExprKind::Unary && static_cast<const UnaryExpr&>(e).op == UnaryOp::Not) {
        return IsSubqueryPredicate(*static_cast<const UnaryExpr&>(e).child);
    }
    return false;
}

void Binder::ApplySubqueryPredicate(LogicalPtr& plan, Scope& /*scope*/, const ParsedExpr& pred,
                                    const ExprContext& ctx) {
    bool negated = false;
    const ParsedExpr* e = &pred;
    while (e->kind == ExprKind::Unary && static_cast<const UnaryExpr&>(*e).op == UnaryOp::Not) {
        negated = !negated;
        e = static_cast<const UnaryExpr&>(*e).child.get();
    }
    if (e->kind == ExprKind::Exists) {
        ApplyExists(plan, *static_cast<const ExistsExpr&>(*e).subquery, negated, ctx, e->pos);
    } else {
        const auto& in = static_cast<const InSubqueryExpr&>(*e);
        ApplyIn(plan, in, negated != in.negated, ctx);
    }
}

namespace {

// EXISTS / IN need only the rows of the subquery that its WHERE clause lets through: grouping,
// aggregation, DISTINCT or LIMIT change which rows exist and are not unnested when correlated.
bool IsPlainBlock(const SelectStatement& sub) {
    if (!sub.group_by.empty() || sub.having || sub.distinct || sub.limit || sub.offset) {
        return false;
    }
    return true;
}

} // namespace

void Binder::ApplyExists(LogicalPtr& plan, const SelectStatement& sub, bool negated,
                         const ExprContext& ctx, size_t pos) {
    CteScope ctes(*this, sub);
    SubqueryCollector inner;
    Block block = BindFromWhere(sub, &ctx, /*allow_correlated=*/true, inner);
    const idx_t lw = plan->ColumnCount();
    auto join = std::make_unique<LogicalJoin>();
    join->join_type = negated ? JoinType::Anti : JoinType::Semi;
    join->names = plan->names;
    join->types = plan->types;
    LogicalPtr right;
    std::vector<BoundExprPtr> conditions;
    if (block.correlated.empty()) {
        // not correlated: EXISTS asks whether the subquery has any row at all
        right = BindSelectBody(sub, &ctx, std::move(block), inner);
        if (PlanContainsOuter(*right)) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated reference outside the WHERE clause of a subquery is not supported "
                 "yet",
                 pos);
        }
    } else {
        if (!IsPlainBlock(sub) || !sub.order_by.empty()) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated EXISTS subquery with GROUP BY, HAVING, DISTINCT or LIMIT is not "
                 "supported yet",
                 pos);
        }
        for (const auto& item : sub.items) { // the select list of an EXISTS is not evaluated
            (void)item;
        }
        Correlation corr = SplitCorrelation(block.correlated);
        for (auto& [inner_expr, outer_expr] : corr.keys) {
            conditions.push_back(
                MakeEquality(std::move(outer_expr), ToJoinCondition(*inner_expr, lw), pos));
        }
        for (const BoundExprPtr& r : corr.residual) {
            conditions.push_back(ToJoinCondition(*r, lw));
        }
        right = std::move(block.plan);
    }
    join->condition = AndAll(std::move(conditions));
    join->children.push_back(std::move(plan));
    join->children.push_back(std::move(right));
    plan = std::move(join);
}

void Binder::ApplyIn(LogicalPtr& plan, const InSubqueryExpr& in, bool negated,
                     const ExprContext& ctx) {
    BoundExprPtr x = BindExpr(*in.child, ctx);
    if (x->ContainsSubqueryValue() || x->ContainsOuterColumn() || x->ContainsAggregate()) {
        Fail(ErrorCode::NotImplemented,
             "the left side of IN (subquery) must be a plain expression of this query block",
             in.pos);
    }
    const SelectStatement& sub = *in.subquery;
    CteScope ctes(*this, sub);
    SubqueryCollector inner;
    Block block = BindFromWhere(sub, &ctx, /*allow_correlated=*/true, inner);
    const idx_t lw = plan->ColumnCount();
    auto join = std::make_unique<LogicalJoin>();
    join->names = plan->names;
    join->types = plan->types;
    LogicalPtr right;
    std::vector<BoundExprPtr> conditions;
    if (block.correlated.empty()) {
        right = BindSelectBody(sub, &ctx, std::move(block), inner);
        if (PlanContainsOuter(*right)) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated reference outside the WHERE clause of a subquery is not supported "
                 "yet",
                 in.pos);
        }
        if (right->ColumnCount() != 1) {
            Fail(ErrorCode::Binder,
                 "the subquery of IN must return one column, not " +
                     std::to_string(right->ColumnCount()),
                 in.pos);
        }
        conditions.push_back(MakeEquality(
            std::move(x), BoundExpr::ColumnRef(lw, right->types[0], right->names[0]), in.pos));
        join->join_type = negated ? JoinType::AntiNullAware : JoinType::Semi;
    } else {
        if (negated) {
            Fail(ErrorCode::NotImplemented,
                 "NOT IN with a correlated subquery is not supported yet (use NOT EXISTS)", in.pos);
        }
        if (!IsPlainBlock(sub) || !sub.order_by.empty() || sub.items.size() != 1 ||
            sub.items[0].expr->kind == ExprKind::Star) {
            Fail(ErrorCode::NotImplemented,
                 "a correlated IN subquery must select one plain expression, without GROUP BY, "
                 "HAVING, DISTINCT or LIMIT",
                 in.pos);
        }
        const ExprContext inner_ctx{&block.scope, false, &ctx, &inner};
        BoundExprPtr y = BindExpr(*sub.items[0].expr, inner_ctx);
        if (y->ContainsOuterColumn() || y->ContainsSubqueryValue() || y->ContainsAggregate()) {
            Fail(ErrorCode::NotImplemented,
                 "the select list of a correlated IN subquery cannot mention outer columns",
                 in.pos);
        }
        conditions.push_back(MakeEquality(std::move(x), ToJoinCondition(*y, lw), in.pos));
        Correlation corr = SplitCorrelation(block.correlated);
        for (auto& [inner_expr, outer_expr] : corr.keys) {
            conditions.push_back(
                MakeEquality(std::move(outer_expr), ToJoinCondition(*inner_expr, lw), in.pos));
        }
        for (const BoundExprPtr& r : corr.residual) {
            conditions.push_back(ToJoinCondition(*r, lw));
        }
        right = std::move(block.plan);
        join->join_type = JoinType::Semi;
    }
    join->condition = AndAll(std::move(conditions));
    join->children.push_back(std::move(plan));
    join->children.push_back(std::move(right));
    plan = std::move(join);
}

} // namespace cdb
