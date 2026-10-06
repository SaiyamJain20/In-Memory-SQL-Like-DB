#include "common/error.h"
#include "planner/binder.h"
#include "planner/scalar_eval.h"
#include "types/cast.h"

#include <algorithm>
#include <cctype>

namespace cdb {

namespace {

// An untyped NULL literal is a Constant with flag=true; it adopts whatever type its context needs.
bool IsUntypedNull(const BoundExpr& e) {
    return e.kind == BoundKind::Constant && e.flag && e.value->IsNull();
}

BoundExprPtr UntypedNull() {
    auto e = BoundExpr::Constant(Value::Null(LogicalType::Integer()));
    e->flag = true;
    return e;
}

bool IsIntegral(LogicalType t) {
    return t.IsIntegral();
}

[[noreturn]] void Fail(ErrorCode code, const std::string& message, size_t pos) {
    throw Error(code, message, pos);
}

// Runs `fn`, re-throwing any Error with `pos` attached when it had none.
template <class F> auto WithPosition(size_t pos, F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const Error& e) {
        if (e.position().has_value())
            throw;
        std::string msg = e.what();
        const std::string prefix = std::string(ErrorCodeName(e.code())) + ": ";
        if (msg.rfind(prefix, 0) == 0)
            msg = msg.substr(prefix.size());
        throw Error(e.code(), msg, pos);
    }
}

// Evaluates a constant tree; if evaluation fails (e.g. overflow in a branch that may never be
// taken) the tree is left for runtime so the error is raised only if the expression is evaluated.
BoundExprPtr TryFold(BoundExprPtr e) {
    if (e->kind == BoundKind::Constant || e->kind == BoundKind::ColumnRef ||
        e->kind == BoundKind::Aggregate || !e->IsConstantTree()) {
        return e;
    }
    try {
        return BoundExpr::Constant(EvaluateConstant(*e));
    } catch (const Error&) {
        return e;
    }
}

// Casts `e` to `target` (no-op when equal), folding constants. Untyped NULL adopts the target.
BoundExprPtr CastTo(BoundExprPtr e, LogicalType target, size_t pos) {
    if (IsUntypedNull(*e))
        return BoundExpr::Constant(Value::Null(target));
    if (e->type == target)
        return e;
    // A NULL constant converts to NULL of any type, even where the non-NULL cast is impossible.
    if (e->kind == BoundKind::Constant && e->value->IsNull())
        return BoundExpr::Constant(Value::Null(target));
    if (!CanCastExplicitly(e->type, target)) {
        Fail(ErrorCode::Type, "Cannot cast " + e->type.ToString() + " to " + target.ToString(),
             pos);
    }
    if (e->kind == BoundKind::Constant) {
        return BoundExpr::Constant(WithPosition(pos, [&] { return CastValue(*e->value, target); }));
    }
    return TryFold(BoundExpr::Cast(std::move(e), target));
}

bool IsVarcharConstant(const BoundExpr& e) {
    return e.kind == BoundKind::Constant && !e.value->IsNull() && e.type.id() == TypeId::Varchar;
}

// Brings two operands to a common type for comparison / arithmetic. A VARCHAR *literal* meeting
// a non-VARCHAR operand is converted to that operand's type (so `date_col < '1998-01-01'` works),
// numbers are promoted, anything else is a type error.
void Unify(BoundExprPtr& l, BoundExprPtr& r, size_t pos, const std::string& op,
           bool convert_string_literals = true) {
    if (IsUntypedNull(*l) && IsUntypedNull(*r)) {
        // NULL <op> NULL has no type information at all; DuckDB settles on BIGINT.
        l = BoundExpr::Constant(Value::Null(LogicalType::BigInt()));
        r = BoundExpr::Constant(Value::Null(LogicalType::BigInt()));
        return;
    }
    if (IsUntypedNull(*l)) {
        l = BoundExpr::Constant(Value::Null(r->type));
        return;
    }
    if (IsUntypedNull(*r)) {
        r = BoundExpr::Constant(Value::Null(l->type));
        return;
    }
    if (l->type == r->type)
        return;
    if (convert_string_literals && IsVarcharConstant(*l) && r->type.id() != TypeId::Varchar) {
        l = CastTo(std::move(l), r->type, pos);
        return;
    }
    if (convert_string_literals && IsVarcharConstant(*r) && l->type.id() != TypeId::Varchar) {
        r = CastTo(std::move(r), l->type, pos);
        return;
    }
    if (auto common = CommonSuperType(l->type, r->type)) {
        l = CastTo(std::move(l), *common, pos);
        r = CastTo(std::move(r), *common, pos);
        return;
    }
    Fail(ErrorCode::Type,
         "No function matches the given name and argument types '" + op + "(" + l->type.ToString() +
             ", " + r->type.ToString() + ")'. You might need to add explicit type casts.",
         pos);
}

BoundExprPtr ExpectBoolean(BoundExprPtr e, size_t pos, const char* what) {
    if (IsUntypedNull(*e))
        return BoundExpr::Constant(Value::Null(LogicalType::Boolean()));
    if (e->type.id() != TypeId::Boolean) {
        if (e->type.IsNumeric()) {
            return CastTo(std::move(e), LogicalType::Boolean(), pos); // implicit: value <> 0
        }
        Fail(ErrorCode::Type,
             std::string(what) + " must be of type BOOLEAN but is " + e->type.ToString(), pos);
    }
    return e;
}

OperatorKind CompareKind(BinaryOp op) {
    switch (op) {
    case BinaryOp::Eq:
        return OperatorKind::Eq;
    case BinaryOp::Ne:
        return OperatorKind::Ne;
    case BinaryOp::Lt:
        return OperatorKind::Lt;
    case BinaryOp::Le:
        return OperatorKind::Le;
    case BinaryOp::Gt:
        return OperatorKind::Gt;
    case BinaryOp::Ge:
        return OperatorKind::Ge;
    default:
        break;
    }
    throw Error(ErrorCode::Internal, "not a comparison");
}

BoundExprPtr MakeComparison(OperatorKind op, BoundExprPtr l, BoundExprPtr r, size_t pos) {
    Unify(l, r, pos, OperatorText(op));
    return TryFold(BoundExpr::Binary(op, std::move(l), std::move(r), LogicalType::Boolean()));
}

std::string Lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

// ------------------------------------------------------------------------------------ names

Binder::Lookup Binder::ResolveColumn(const Scope& scope, const std::string& table,
                                     const std::string& column, idx_t* ordinal) {
    idx_t found = 0;
    int matches = 0;
    for (idx_t i = 0; i < scope.columns.size(); i++) {
        const ScopeColumn& c = scope.columns[i];
        if (c.name != column)
            continue;
        if (!table.empty()) {
            if (c.table != table)
                continue;
        } else if (c.hidden) {
            continue;
        }
        found = i;
        matches++;
    }
    if (matches == 0)
        return Lookup::NotFound;
    if (matches > 1)
        return Lookup::Ambiguous;
    *ordinal = found;
    return Lookup::Found;
}

BoundExprPtr Binder::BindColumnRef(const ColumnRefExpr& e, const ExprContext& ctx) {
    if (BoundExprPtr bound = TryBindColumnRef(e, ctx)) {
        return bound;
    }
    // Not found in any block. A qualified name is blamed on the innermost block that has a table
    // of that name (`t.nope`: no column "nope" in table "t"); otherwise on the block it was written
    // in.
    const std::string table = Lower(e.table), column = Lower(e.column);
    if (!table.empty()) {
        for (const ExprContext* c = &ctx; c != nullptr; c = c->outer) {
            if (c->scope != nullptr &&
                std::any_of(c->scope->columns.begin(), c->scope->columns.end(),
                            [&](const ScopeColumn& col) { return col.table == table; })) {
                Fail(ErrorCode::Binder,
                     "Referenced column \"" + column + "\" not found in table \"" + table + "\"",
                     e.pos);
            }
        }
        Fail(ErrorCode::Binder, "Referenced table \"" + table + "\" not found in FROM clause",
             e.pos);
    }
    Fail(ErrorCode::Binder, "Referenced column \"" + column + "\" not found in FROM clause", e.pos);
}

BoundExprPtr Binder::TryBindColumnRef(const ColumnRefExpr& e, const ExprContext& ctx) {
    static const Scope kEmpty;
    const Scope& scope = ctx.scope ? *ctx.scope : kEmpty;
    const std::string table = Lower(e.table), column = Lower(e.column);
    idx_t ordinal = 0;
    switch (ResolveColumn(scope, table, column, &ordinal)) {
    case Lookup::Found: {
        const ScopeColumn& c = scope.columns[ordinal];
        // Plans show `t.a` only where a bare `a` would be ambiguous (e.g. in a join condition).
        int same_name = 0;
        for (const ScopeColumn& other : scope.columns) {
            same_name += other.name == c.name;
        }
        const std::string display =
            (same_name > 1 && !c.table.empty()) ? c.table + "." + c.name : c.name;
        return BoundExpr::ColumnRef(ordinal, c.type, display);
    }
    case Lookup::Ambiguous: {
        std::string options;
        for (const ScopeColumn& c : scope.columns) {
            if (c.name == column && !c.hidden) {
                options += std::string(options.empty() ? "" : " or ") + "\"" + c.table + "." +
                           c.name + "\"";
            }
        }
        Fail(ErrorCode::Binder,
             "Ambiguous reference to column name \"" + column + "\" (use: " + options + ")", e.pos);
    }
    case Lookup::NotFound:
        break;
    }
    if (ctx.outer == nullptr) {
        return nullptr;
    }
    // not a column of this block: it may belong to the enclosing one (a correlated reference)
    BoundExprPtr outer = TryBindColumnRef(e, *ctx.outer);
    if (outer == nullptr) {
        return nullptr;
    }
    if (outer->kind == BoundKind::OuterColumn) {
        Fail(ErrorCode::NotImplemented,
             "a subquery referring to a column two query levels up is not supported yet", e.pos);
    }
    return BoundExpr::OuterColumn(outer->ordinal, outer->type, outer->name);
}

// ------------------------------------------------------------------------------ expressions

BoundExprPtr Binder::BindExpr(const ParsedExpr& e, const ExprContext& ctx) {
    switch (e.kind) {
    case ExprKind::ColumnRef:
        return BindColumnRef(static_cast<const ColumnRefExpr&>(e), ctx);
    case ExprKind::Star:
        Fail(ErrorCode::Binder, "\"*\" is not allowed in this context", e.pos);
    case ExprKind::Constant:
        return BindConstant(static_cast<const ConstantExpr&>(e));
    case ExprKind::Interval:
        Fail(ErrorCode::NotImplemented,
             "INTERVAL values are only supported in DATE +/- INTERVAL arithmetic", e.pos);
    case ExprKind::Unary:
        return BindUnary(static_cast<const UnaryExpr&>(e), ctx);
    case ExprKind::Binary:
        return BindBinary(static_cast<const BinaryExpr&>(e), ctx);
    case ExprKind::IsNull: {
        const auto& n = static_cast<const IsNullExpr&>(e);
        return TryFold(BoundExpr::IsNull(BindExpr(*n.child, ctx), n.negated));
    }
    case ExprKind::Between:
        return BindBetween(static_cast<const BetweenExpr&>(e), ctx);
    case ExprKind::InList:
        return BindInList(static_cast<const InListExpr&>(e), ctx);
    case ExprKind::ScalarSubquery:
        return BindScalarSubquery(static_cast<const ScalarSubqueryExpr&>(e), ctx);
    case ExprKind::InSubquery:
    case ExprKind::Exists:
        Fail(ErrorCode::NotImplemented,
             "EXISTS and IN subqueries are only supported as AND-ed conditions of a WHERE clause",
             e.pos);
    case ExprKind::Like:
        return BindLike(static_cast<const LikeExpr&>(e), ctx);
    case ExprKind::Case:
        return BindCase(static_cast<const CaseExpr&>(e), ctx);
    case ExprKind::Cast:
        return BindCast(static_cast<const CastExpr&>(e), ctx);
    case ExprKind::Function:
        return BindFunction(static_cast<const FunctionExpr&>(e), ctx);
    case ExprKind::Extract:
        return BindExtract(static_cast<const ExtractExpr&>(e), ctx);
    }
    throw Error(ErrorCode::Internal, "unknown expression kind");
}

BoundExprPtr Binder::BindConstant(const ConstantExpr& e) {
    if (e.literal == LiteralKind::Null)
        return UntypedNull();
    return BoundExpr::Constant(e.value);
}

BoundExprPtr Binder::BindUnary(const UnaryExpr& e, const ExprContext& ctx) {
    BoundExprPtr child = BindExpr(*e.child, ctx);
    if (e.op == UnaryOp::Not) {
        child = ExpectBoolean(std::move(child), e.pos, "Argument of NOT");
        return TryFold(
            BoundExpr::Unary(OperatorKind::Not, std::move(child), LogicalType::Boolean()));
    }
    if (IsUntypedNull(*child))
        return BoundExpr::Constant(Value::Null(LogicalType::BigInt())); // like DuckDB
    if (!child->type.IsNumeric()) {
        Fail(ErrorCode::Type,
             "No function matches the given name and argument types '-(" + child->type.ToString() +
                 ")'. You might need to add explicit type casts.",
             e.pos);
    }
    const LogicalType t = child->type;
    return TryFold(BoundExpr::Unary(OperatorKind::Negate, std::move(child), t));
}

BoundExprPtr Binder::BindDateInterval(const BinaryExpr& e, const ExprContext& ctx) {
    // exactly one side is an IntervalExpr; the other must fold to a DATE constant
    const bool interval_right = e.right->kind == ExprKind::Interval;
    const auto& interval = static_cast<const IntervalExpr&>(interval_right ? *e.right : *e.left);
    const ParsedExpr& other_ast = interval_right ? *e.left : *e.right;
    if (!interval_right && e.op != BinaryOp::Add) {
        Fail(ErrorCode::Type, "INTERVAL cannot be the left operand of '-'", e.pos);
    }
    if (e.op != BinaryOp::Add && e.op != BinaryOp::Sub) {
        Fail(ErrorCode::Type, "INTERVAL only supports + and -", e.pos);
    }
    BoundExprPtr base = BindExpr(other_ast, ctx);
    if (IsVarcharConstant(*base))
        base = CastTo(std::move(base), LogicalType::Date(), e.pos);
    if (base->kind != BoundKind::Constant || base->type.id() != TypeId::Date) {
        Fail(base->type.id() == TypeId::Date ? ErrorCode::NotImplemented : ErrorCode::Type,
             base->type.id() == TypeId::Date
                 ? "INTERVAL arithmetic is only supported on constant dates"
                 : "INTERVAL can only be added to / subtracted from a DATE",
             e.pos);
    }
    if (base->value->IsNull())
        return BoundExpr::Constant(Value::Null(LogicalType::Date()));
    const int64_t amount = e.op == BinaryOp::Sub ? -interval.amount : interval.amount;
    return BoundExpr::Constant(Value::Date(WithPosition(
        e.pos, [&] { return AddInterval(base->value->GetDate(), amount, interval.unit); })));
}

BoundExprPtr Binder::BindBinary(const BinaryExpr& e, const ExprContext& ctx) {
    if (e.left->kind == ExprKind::Interval || e.right->kind == ExprKind::Interval) {
        return BindDateInterval(e, ctx);
    }
    if (e.op == BinaryOp::And || e.op == BinaryOp::Or) {
        BoundExprPtr l =
            ExpectBoolean(BindExpr(*e.left, ctx), e.left->pos,
                          e.op == BinaryOp::And ? "Argument of AND" : "Argument of OR");
        BoundExprPtr r =
            ExpectBoolean(BindExpr(*e.right, ctx), e.right->pos,
                          e.op == BinaryOp::And ? "Argument of AND" : "Argument of OR");
        return TryFold(
            BoundExpr::Binary(e.op == BinaryOp::And ? OperatorKind::And : OperatorKind::Or,
                              std::move(l), std::move(r), LogicalType::Boolean()));
    }
    BoundExprPtr l = BindExpr(*e.left, ctx);
    BoundExprPtr r = BindExpr(*e.right, ctx);

    switch (e.op) {
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
        return MakeComparison(CompareKind(e.op), std::move(l), std::move(r), e.pos);
    case BinaryOp::Concat: {
        l = CastTo(std::move(l), LogicalType::Varchar(), e.pos);
        r = CastTo(std::move(r), LogicalType::Varchar(), e.pos);
        return TryFold(BoundExpr::Binary(OperatorKind::Concat, std::move(l), std::move(r),
                                         LogicalType::Varchar()));
    }
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Mod:
        break;
    default:
        throw Error(ErrorCode::Internal, "unhandled binary operator");
    }

    const OperatorKind op = e.op == BinaryOp::Add   ? OperatorKind::Add
                            : e.op == BinaryOp::Sub ? OperatorKind::Sub
                            : e.op == BinaryOp::Mul ? OperatorKind::Mul
                            : e.op == BinaryOp::Div ? OperatorKind::Div
                                                    : OperatorKind::Mod;
    // DATE +/- integer days, integer + DATE, DATE - DATE
    const bool l_date = l->type.id() == TypeId::Date && !IsUntypedNull(*l);
    const bool r_date = r->type.id() == TypeId::Date && !IsUntypedNull(*r);
    if (l_date || r_date) {
        // DATE +/- INTEGER days only (BIGINT is rejected, like DuckDB)
        auto days = [](const BoundExpr& x) { return x.type.id() == TypeId::Integer; };
        const bool ok =
            (op == OperatorKind::Add && ((l_date && days(*r)) || (r_date && days(*l)))) ||
            (op == OperatorKind::Sub && l_date && (days(*r) || r_date));
        if (!ok) {
            Fail(ErrorCode::Type,
                 std::string("No function matches the given name and argument types '") +
                     OperatorText(op) + "(" + l->type.ToString() + ", " + r->type.ToString() +
                     ")'. You might need to add explicit type casts.",
                 e.pos);
        }
        const LogicalType result = (l_date && r_date) ? LogicalType::BigInt() : LogicalType::Date();
        return TryFold(BoundExpr::Binary(op, std::move(l), std::move(r), result));
    }

    Unify(l, r, e.pos, OperatorText(op), /*convert_string_literals=*/false);
    if (!l->type.IsNumeric()) {
        Fail(ErrorCode::Type,
             std::string("No function matches the given name and argument types '") +
                 OperatorText(op) + "(" + l->type.ToString() + ", " + r->type.ToString() +
                 ")'. You might need to add explicit type casts.",
             e.pos);
    }
    LogicalType result = l->type;
    if (op == OperatorKind::Div) { // `/` is always floating point, like DuckDB
        l = CastTo(std::move(l), LogicalType::Double(), e.pos);
        r = CastTo(std::move(r), LogicalType::Double(), e.pos);
        result = LogicalType::Double();
    }
    return TryFold(BoundExpr::Binary(op, std::move(l), std::move(r), result));
}

BoundExprPtr Binder::BindBetween(const BetweenExpr& e, const ExprContext& ctx) {
    BoundExprPtr x = BindExpr(*e.child, ctx);
    BoundExprPtr lo = BindExpr(*e.lower, ctx);
    BoundExprPtr hi = BindExpr(*e.upper, ctx);
    // x BETWEEN lo AND hi  ==  x >= lo AND x <= hi   (x is evaluated twice)
    BoundExprPtr ge = MakeComparison(OperatorKind::Ge, x->Clone(), std::move(lo), e.pos);
    BoundExprPtr le = MakeComparison(OperatorKind::Le, std::move(x), std::move(hi), e.pos);
    BoundExprPtr both = TryFold(
        BoundExpr::Binary(OperatorKind::And, std::move(ge), std::move(le), LogicalType::Boolean()));
    if (!e.negated)
        return both;
    return TryFold(BoundExpr::Unary(OperatorKind::Not, std::move(both), LogicalType::Boolean()));
}

BoundExprPtr Binder::BindInList(const InListExpr& e, const ExprContext& ctx) {
    BoundExprPtr value = BindExpr(*e.child, ctx);
    std::vector<BoundExprPtr> items;
    for (const auto& item : e.list)
        items.push_back(BindExpr(*item, ctx));

    // One common type for the value and every list element.
    LogicalType common = value->type;
    bool have_type = !IsUntypedNull(*value);
    for (const auto& item : items) {
        if (IsUntypedNull(*item))
            continue;
        if (!have_type) {
            common = item->type;
            have_type = true;
            continue;
        }
        if (item->type == common)
            continue;
        if (IsVarcharConstant(*item) && common.id() != TypeId::Varchar)
            continue; // cast to `common`
        auto super = CommonSuperType(common, item->type);
        if (!super) {
            Fail(ErrorCode::Type,
                 "Cannot compare values of type " + common.ToString() + " and type " +
                     item->type.ToString() + " in IN list",
                 e.pos);
        }
        common = *super;
    }
    if (!have_type)
        common = LogicalType::Integer();
    value = CastTo(std::move(value), common, e.pos);
    for (auto& item : items)
        item = CastTo(std::move(item), common, e.pos);
    return TryFold(BoundExpr::InList(std::move(value), std::move(items), e.negated));
}

BoundExprPtr Binder::BindLike(const LikeExpr& e, const ExprContext& ctx) {
    BoundExprPtr text = BindExpr(*e.child, ctx);
    BoundExprPtr pattern = BindExpr(*e.pattern, ctx);
    for (BoundExprPtr* arg : {&text, &pattern}) {
        if (IsUntypedNull(**arg)) {
            *arg = BoundExpr::Constant(Value::Null(LogicalType::Varchar()));
        } else if ((*arg)->type.id() != TypeId::Varchar) {
            Fail(ErrorCode::Type,
                 "No function matches the given name and argument types 'like(" +
                     text->type.ToString() + ", " + pattern->type.ToString() +
                     ")'. LIKE needs VARCHAR operands.",
                 e.pos);
        }
    }
    std::vector<BoundExprPtr> args;
    args.push_back(std::move(text));
    args.push_back(std::move(pattern));
    BoundExprPtr like =
        TryFold(BoundExpr::Call(FunctionId::Like, std::move(args), LogicalType::Boolean()));
    if (!e.negated)
        return like;
    return TryFold(BoundExpr::Unary(OperatorKind::Not, std::move(like), LogicalType::Boolean()));
}

BoundExprPtr Binder::BindCase(const CaseExpr& e, const ExprContext& ctx) {
    std::vector<BoundExprPtr> conditions, results;
    BoundExprPtr operand = e.operand ? BindExpr(*e.operand, ctx) : nullptr;
    for (const auto& w : e.whens) {
        BoundExprPtr cond = BindExpr(*w.condition, ctx);
        if (operand) {
            cond = MakeComparison(OperatorKind::Eq, operand->Clone(), std::move(cond),
                                  w.condition->pos);
        } else {
            cond = ExpectBoolean(std::move(cond), w.condition->pos, "CASE condition");
        }
        conditions.push_back(std::move(cond));
        results.push_back(BindExpr(*w.result, ctx));
    }
    BoundExprPtr else_result = e.else_result ? BindExpr(*e.else_result, ctx) : UntypedNull();

    // unify the result type across THEN branches and ELSE
    std::optional<LogicalType> common;
    auto fold_in = [&](const BoundExpr& r) {
        if (IsUntypedNull(r))
            return;
        if (!common) {
            common = r.type;
            return;
        }
        auto super = CommonSuperType(*common, r.type);
        if (!super) {
            Fail(ErrorCode::Type,
                 "CASE results must have a common type, but found " + common->ToString() + " and " +
                     r.type.ToString(),
                 e.pos);
        }
        common = *super;
    };
    for (const auto& r : results)
        fold_in(*r);
    fold_in(*else_result);
    const LogicalType result_type = common.value_or(LogicalType::Integer());

    std::vector<BoundExprPtr> children;
    for (size_t i = 0; i < conditions.size(); i++) {
        children.push_back(std::move(conditions[i]));
        children.push_back(CastTo(std::move(results[i]), result_type, e.pos));
    }
    children.push_back(CastTo(std::move(else_result), result_type, e.pos));
    return TryFold(BoundExpr::Case(std::move(children), result_type));
}

BoundExprPtr Binder::BindCast(const CastExpr& e, const ExprContext& ctx) {
    return CastTo(BindExpr(*e.child, ctx), e.target, e.pos);
}

BoundExprPtr Binder::BindExtract(const ExtractExpr& e, const ExprContext& ctx) {
    static const std::pair<const char*, const char*> kFields[] = {
        {"year", "year"},           {"month", "month"},         {"day", "day"},
        {"dow", "dayofweek"},       {"dayofweek", "dayofweek"}, {"doy", "dayofyear"},
        {"dayofyear", "dayofyear"}, {"quarter", "quarter"}};
    for (const auto& [field, fn] : kFields) {
        if (e.field == field) {
            std::vector<BoundExprPtr> args;
            args.push_back(BindExpr(*e.child, ctx));
            return BindScalarFunction(fn, std::move(args), e.pos);
        }
    }
    Fail(ErrorCode::NotImplemented, "EXTRACT field \"" + e.field + "\" is not supported", e.pos);
}

// ------------------------------------------------------------------------------- functions

BoundExprPtr Binder::BindFunction(const FunctionExpr& e, const ExprContext& ctx) {
    static const char* const kAggregates[] = {"count", "sum", "avg", "min", "max"};
    if (std::find(std::begin(kAggregates), std::end(kAggregates), e.name) !=
        std::end(kAggregates)) {
        return BindAggregate(e, ctx);
    }
    if (e.star || e.distinct) {
        Fail(ErrorCode::Binder, "function \"" + e.name + "\" does not accept * or DISTINCT", e.pos);
    }
    std::vector<BoundExprPtr> args;
    for (const auto& a : e.args)
        args.push_back(BindExpr(*a, ctx));
    return BindScalarFunction(e.name, std::move(args), e.pos);
}

BoundExprPtr Binder::BindAggregate(const FunctionExpr& e, const ExprContext& ctx) {
    if (!ctx.allow_aggregates) {
        Fail(ErrorCode::Binder,
             "aggregate function \"" + e.name + "\" is not allowed in this context", e.pos);
    }
    if (e.star) {
        if (e.name != "count")
            Fail(ErrorCode::Binder, "\"*\" is only valid in count(*)", e.pos);
        if (e.distinct)
            Fail(ErrorCode::Binder, "count(DISTINCT *) is not valid", e.pos);
        return BoundExpr::Aggregate(AggregateKind::CountStar, {}, false, LogicalType::BigInt());
    }
    if (e.args.size() != 1) {
        Fail(ErrorCode::Binder,
             "aggregate \"" + e.name + "\" expects 1 argument but " +
                 std::to_string(e.args.size()) + " were given",
             e.pos);
    }
    // arguments are plain (non-aggregate) expressions
    ExprContext inner{ctx.scope, /*allow_aggregates=*/false};
    BoundExprPtr arg = BindExpr(*e.args[0], inner);
    if (IsUntypedNull(*arg))
        arg = BoundExpr::Constant(Value::Null(LogicalType::Integer()));
    const LogicalType t = arg->type;

    AggregateKind kind;
    LogicalType result = t;
    if (e.name == "count") {
        kind = AggregateKind::Count;
        result = LogicalType::BigInt();
    } else if (e.name == "sum") {
        kind = AggregateKind::Sum;
        if (!t.IsNumeric()) {
            Fail(ErrorCode::Type, "sum() requires a numeric argument but got " + t.ToString(),
                 e.pos);
        }
        result = t.id() == TypeId::Double ? LogicalType::Double() : LogicalType::BigInt();
        if (t.id() == TypeId::Integer)
            arg = CastTo(std::move(arg), LogicalType::BigInt(), e.pos);
    } else if (e.name == "avg") {
        kind = AggregateKind::Avg;
        if (!t.IsNumeric()) {
            Fail(ErrorCode::Type, "avg() requires a numeric argument but got " + t.ToString(),
                 e.pos);
        }
        result = LogicalType::Double();
        arg = CastTo(std::move(arg), LogicalType::Double(), e.pos);
    } else {
        kind = e.name == "min" ? AggregateKind::Min : AggregateKind::Max;
    }
    std::vector<BoundExprPtr> args;
    args.push_back(std::move(arg));
    return BoundExpr::Aggregate(kind, std::move(args), e.distinct, result);
}

BoundExprPtr Binder::BindScalarFunction(const std::string& name, std::vector<BoundExprPtr> args,
                                        size_t pos) {
    auto arity = [&](size_t lo, size_t hi) {
        if (args.size() < lo || args.size() > hi) {
            Fail(ErrorCode::Binder,
                 "function \"" + name + "\" expects " +
                     (lo == hi ? std::to_string(lo)
                               : std::to_string(lo) + " to " + std::to_string(hi)) +
                     " argument(s) but " + std::to_string(args.size()) + " were given",
                 pos);
        }
    };
    auto bad_type = [&](size_t i, const char* wanted) {
        Fail(ErrorCode::Type,
             "No function matches the given name and argument types '" + name + "(" +
                 args[i]->type.ToString() + ")'. Argument " + std::to_string(i + 1) + " must be " +
                 wanted + ".",
             pos);
    };
    auto as_varchar = [&](size_t i) {
        if (IsUntypedNull(*args[i]))
            args[i] = BoundExpr::Constant(Value::Null(LogicalType::Varchar()));
        if (args[i]->type.id() != TypeId::Varchar)
            bad_type(i, "VARCHAR");
    };
    auto as_integral = [&](size_t i) {
        if (IsUntypedNull(*args[i]))
            args[i] = BoundExpr::Constant(Value::Null(LogicalType::BigInt()));
        if (!IsIntegral(args[i]->type))
            bad_type(i, "an integer");
    };
    auto as_double = [&](size_t i) {
        if (IsUntypedNull(*args[i]))
            args[i] = BoundExpr::Constant(Value::Null(LogicalType::Double()));
        if (!args[i]->type.IsNumeric())
            bad_type(i, "numeric");
        args[i] = CastTo(std::move(args[i]), LogicalType::Double(), pos);
    };
    auto as_date = [&](size_t i) {
        // strict, like DuckDB: neither a string literal nor an untyped NULL is converted
        if (IsUntypedNull(*args[i]) || args[i]->type.id() != TypeId::Date)
            bad_type(i, "a DATE");
    };
    auto call = [&](FunctionId id, LogicalType type) {
        return TryFold(BoundExpr::Call(id, std::move(args), type));
    };

    if (name == "year" || name == "month" || name == "day" || name == "dayofweek" ||
        name == "dayofyear" || name == "quarter") {
        arity(1, 1);
        as_date(0);
        const FunctionId id = name == "year"        ? FunctionId::Year
                              : name == "month"     ? FunctionId::Month
                              : name == "day"       ? FunctionId::Day
                              : name == "dayofweek" ? FunctionId::DayOfWeek
                              : name == "dayofyear" ? FunctionId::DayOfYear
                                                    : FunctionId::Quarter;
        return call(id, LogicalType::BigInt());
    }
    if (name == "upper" || name == "lower") {
        arity(1, 1);
        as_varchar(0);
        return call(name == "upper" ? FunctionId::Upper : FunctionId::Lower,
                    LogicalType::Varchar());
    }
    if (name == "length") {
        arity(1, 1);
        as_varchar(0);
        return call(FunctionId::Length, LogicalType::BigInt());
    }
    if (name == "substring" || name == "substr") {
        arity(2, 3);
        as_varchar(0);
        as_integral(1);
        if (args.size() > 2)
            as_integral(2);
        return call(FunctionId::Substring, LogicalType::Varchar());
    }
    if (name == "abs") {
        arity(1, 1);
        if (IsUntypedNull(*args[0]))
            return BoundExpr::Constant(Value::Null(LogicalType::BigInt()));
        if (!args[0]->type.IsNumeric())
            bad_type(0, "numeric");
        const LogicalType t = args[0]->type;
        return call(FunctionId::Abs, t);
    }
    if (name == "round") {
        arity(1, 2);
        if (IsUntypedNull(*args[0]))
            args[0] = BoundExpr::Constant(Value::Null(LogicalType::BigInt()));
        // ROUND of an integer stays an integer (only negative digits change it), like DuckDB
        const bool integral = IsIntegral(args[0]->type);
        const LogicalType result = integral ? args[0]->type : LogicalType(LogicalType::Double());
        if (!integral)
            as_double(0);
        if (args.size() > 1)
            as_integral(1);
        return call(FunctionId::Round, result);
    }
    if (name == "floor" || name == "ceil" || name == "ceiling") {
        arity(1, 1);
        as_double(0);
        return call(name == "floor" ? FunctionId::Floor : FunctionId::Ceil, LogicalType::Double());
    }
    if (name == "coalesce") {
        arity(1, 64);
        std::optional<LogicalType> common;
        for (const auto& a : args) {
            if (IsUntypedNull(*a))
                continue;
            if (!common) {
                common = a->type;
                continue;
            }
            auto super = CommonSuperType(*common, a->type);
            if (!super) {
                Fail(ErrorCode::Type,
                     "coalesce arguments must have a common type, but found " + common->ToString() +
                         " and " + a->type.ToString(),
                     pos);
            }
            common = *super;
        }
        const LogicalType t = common.value_or(LogicalType::Integer());
        for (auto& a : args)
            a = CastTo(std::move(a), t, pos);
        return call(FunctionId::Coalesce, t);
    }
    if (name == "nullif") {
        arity(2, 2);
        // The result keeps the FIRST argument's type (it is either that value or NULL); the
        // comparison happens at the common type.
        const LogicalType original = IsUntypedNull(*args[0]) ? args[1]->type : args[0]->type;
        Unify(args[0], args[1], pos, "nullif");
        const LogicalType common = args[0]->type;
        BoundExprPtr result = call(FunctionId::NullIf, common);
        return common == original ? std::move(result) : CastTo(std::move(result), original, pos);
    }
    Fail(ErrorCode::Binder, "function \"" + name + "\" does not exist", pos);
}

} // namespace cdb
