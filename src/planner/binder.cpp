#include "planner/binder.h"

#include "common/error.h"
#include "planner/scalar_eval.h"
#include "types/cast.h"

#include <algorithm>
#include <cctype>
#include <map>

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

// Output-column name for an unaliased select item: the column name for a bare reference,
// otherwise the expression's SQL text without its outermost parentheses.
std::string DisplayName(const ParsedExpr& e) {
    if (e.kind == ExprKind::ColumnRef)
        return static_cast<const ColumnRefExpr&>(e).column;
    std::string s = e.ToString();
    if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
        int depth = 0;
        bool wraps = true;
        for (size_t i = 0; i + 1 < s.size(); i++) {
            if (s[i] == '(')
                depth++;
            if (s[i] == ')')
                depth--;
            if (depth == 0) {
                wraps = false;
                break;
            }
        }
        if (wraps)
            s = s.substr(1, s.size() - 2);
    }
    return s;
}

bool IsUntypedNullConst(const BoundExpr& e) {
    return e.kind == BoundKind::Constant && e.flag && e.value->IsNull();
}

// Assignment cast used by INSERT: like CAST, with an untyped NULL adopting the column type.
BoundExprPtr AssignCast(BoundExprPtr e, LogicalType target, size_t pos) {
    if (IsUntypedNullConst(*e))
        return BoundExpr::Constant(Value::Null(target));
    if (e->type == target)
        return e;
    if (!CanCastExplicitly(e->type, target)) {
        Fail(ErrorCode::Type, "Cannot cast " + e->type.ToString() + " to " + target.ToString(),
             pos);
    }
    if (e->IsConstantTree()) {
        try {
            return BoundExpr::Constant(CastValue(EvaluateConstant(*e), target));
        } catch (const Error& err) {
            if (err.position().has_value())
                throw;
            std::string msg = err.what();
            const std::string prefix = std::string(ErrorCodeName(err.code())) + ": ";
            if (msg.rfind(prefix, 0) == 0)
                msg = msg.substr(prefix.size());
            throw Error(err.code(), msg, pos);
        }
    }
    return BoundExpr::Cast(std::move(e), target);
}

void FillOutput(LogicalOperator& op, std::vector<std::string> names,
                std::vector<LogicalType> types) {
    op.names = std::move(names);
    op.types = std::move(types);
}

} // namespace

LogicalPtr Binder::Bind(const Statement& statement) {
    switch (statement.kind) {
    case StatementKind::Select:
        return BindSelect(static_cast<const SelectStatement&>(statement));
    case StatementKind::CreateTable:
        return BindCreateTable(static_cast<const CreateTableStatement&>(statement));
    case StatementKind::DropTable:
        return BindDropTable(static_cast<const DropTableStatement&>(statement));
    case StatementKind::Insert:
        return BindInsert(static_cast<const InsertStatement&>(statement));
    case StatementKind::Copy:
        return BindCopy(static_cast<const CopyStatement&>(statement));
    case StatementKind::Explain:
        return BindExplain(static_cast<const ExplainStatement&>(statement));
    case StatementKind::Checkpoint:
        return std::make_unique<LogicalCheckpoint>();
    }
    throw Error(ErrorCode::Internal, "unknown statement kind");
}

// ------------------------------------------------------------------------------- DDL / DML

LogicalPtr Binder::BindCreateTable(const CreateTableStatement& stmt) {
    auto op = std::make_unique<LogicalCreateTable>();
    op->table_name = stmt.name;
    op->if_not_exists = stmt.if_not_exists;
    for (const ColumnSpec& c : stmt.columns) {
        op->schema.push_back(ColumnDefinition{c.name, c.type, c.not_null});
    }
    return op;
}

LogicalPtr Binder::BindDropTable(const DropTableStatement& stmt) {
    auto op = std::make_unique<LogicalDropTable>();
    op->table_name = stmt.name;
    op->if_exists = stmt.if_exists;
    return op;
}

LogicalPtr Binder::BindInsert(const InsertStatement& stmt) {
    std::shared_ptr<Table> table = catalog_.GetTable(stmt.table);
    const auto& schema = table->schema();

    // target[i] = table column receiving the i-th supplied value
    std::vector<idx_t> target;
    if (stmt.columns.empty()) {
        for (idx_t i = 0; i < schema.size(); i++)
            target.push_back(i);
    } else {
        for (const std::string& name : stmt.columns) {
            auto col = table->FindColumn(name);
            if (!col) {
                throw Error(ErrorCode::Binder,
                            "table \"" + table->name() + "\" has no column \"" + name + "\"");
            }
            if (std::find(target.begin(), target.end(), *col) != target.end()) {
                throw Error(ErrorCode::Binder, "column \"" + name + "\" specified more than once");
            }
            target.push_back(*col);
        }
    }
    std::vector<std::string> names;
    std::vector<LogicalType> types;
    for (const auto& c : schema) {
        names.push_back(c.name);
        types.push_back(c.type);
    }
    // source_of[table col] = index into the supplied values, or npos for omitted columns (NULL)
    std::vector<size_t> source_of(schema.size(), std::string::npos);
    for (size_t i = 0; i < target.size(); i++)
        source_of[target[i]] = i;

    LogicalPtr source;
    if (stmt.select) {
        LogicalPtr child = BindSelect(*stmt.select);
        if (child->ColumnCount() != target.size()) {
            throw Error(ErrorCode::Binder, "INSERT has " + std::to_string(target.size()) +
                                               " target column(s) but the query returns " +
                                               std::to_string(child->ColumnCount()));
        }
        auto proj = std::make_unique<LogicalProjection>();
        for (idx_t j = 0; j < schema.size(); j++) {
            if (source_of[j] == std::string::npos) {
                proj->exprs.push_back(BoundExpr::Constant(Value::Null(schema[j].type)));
            } else {
                const idx_t s = source_of[j];
                proj->exprs.push_back(AssignCast(
                    BoundExpr::ColumnRef(s, child->types[s], child->names[s]), schema[j].type, 0));
            }
        }
        FillOutput(*proj, names, types);
        proj->children.push_back(std::move(child));
        source = std::move(proj);
    } else {
        auto values = std::make_unique<LogicalValues>();
        Scope empty;
        ExprContext ctx{&empty, false};
        for (const auto& row : stmt.rows) {
            if (row.size() != target.size()) {
                throw Error(ErrorCode::Binder,
                            "INSERT expects " + std::to_string(target.size()) +
                                " value(s) per row but a row has " + std::to_string(row.size()),
                            row.empty() ? 0 : row[0]->pos);
            }
            std::vector<BoundExprPtr> bound;
            for (idx_t j = 0; j < schema.size(); j++) {
                if (source_of[j] == std::string::npos) {
                    bound.push_back(BoundExpr::Constant(Value::Null(schema[j].type)));
                } else {
                    const ParsedExpr& v = *row[source_of[j]];
                    bound.push_back(AssignCast(BindExpr(v, ctx), schema[j].type, v.pos));
                }
            }
            values->rows.push_back(std::move(bound));
        }
        FillOutput(*values, names, types);
        source = std::move(values);
    }
    auto insert = std::make_unique<LogicalInsert>();
    insert->table = std::move(table);
    FillOutput(*insert, {"Count"}, {LogicalType::BigInt()});
    insert->children.push_back(std::move(source));
    return insert;
}

LogicalPtr Binder::BindCopy(const CopyStatement& stmt) {
    auto op = std::make_unique<LogicalCopy>();
    op->table = catalog_.GetTable(stmt.table);
    op->path = stmt.path;
    op->delimiter = stmt.delimiter;
    op->header = stmt.header;
    FillOutput(*op, {"Count"}, {LogicalType::BigInt()});
    return op;
}

LogicalPtr Binder::BindExplain(const ExplainStatement& stmt) {
    auto op = std::make_unique<LogicalExplain>();
    op->analyze = stmt.analyze;
    FillOutput(*op, {"explain_value"}, {LogicalType::Varchar()});
    op->children.push_back(Bind(*stmt.inner));
    return op;
}

// --------------------------------------------------------------------------------- FROM

Binder::BoundTable Binder::BindTableRef(const TableRef& ref) {
    switch (ref.kind) {
    case TableRefKind::Base:
        return BindBaseTable(static_cast<const BaseTableRef&>(ref));
    case TableRefKind::Join:
        return BindJoin(static_cast<const JoinRef&>(ref));
    case TableRefKind::Subquery:
        return BindSubqueryRef(static_cast<const SubqueryRef&>(ref));
    }
    throw Error(ErrorCode::Internal, "unknown table ref kind");
}

Binder::BoundTable Binder::BindBaseTable(const BaseTableRef& ref) {
    std::shared_ptr<Table> table = catalog_.TryGetTable(ref.name);
    if (!table) {
        Fail(ErrorCode::Catalog, "table \"" + ref.name + "\" does not exist", ref.pos);
    }
    auto get = std::make_unique<LogicalGet>();
    BoundTable out;
    const std::string alias = Lower(ref.alias.empty() ? table->name() : ref.alias);
    if (ref.column_aliases.size() > table->schema().size()) {
        Fail(ErrorCode::Binder,
             "table \"" + alias + "\" has " + std::to_string(table->schema().size()) +
                 " columns but " + std::to_string(ref.column_aliases.size()) +
                 " column aliases were given",
             ref.pos);
    }
    for (idx_t i = 0; i < table->schema().size(); i++) {
        const auto& col = table->schema()[i];
        const std::string name =
            Lower(i < ref.column_aliases.size() ? ref.column_aliases[i] : col.name);
        get->column_ids.push_back(i);
        get->names.push_back(name);
        get->types.push_back(col.type);
        out.scope.columns.push_back({alias, name, col.type, false});
    }
    get->table = std::move(table);
    out.plan = std::move(get);
    return out;
}

Binder::BoundTable Binder::BindSubqueryRef(const SubqueryRef& ref) {
    BoundTable out;
    out.plan = BindSelect(*ref.select);
    if (ref.column_aliases.size() > out.plan->ColumnCount()) {
        Fail(ErrorCode::Binder,
             "subquery \"" + ref.alias + "\" has " + std::to_string(out.plan->ColumnCount()) +
                 " columns but " + std::to_string(ref.column_aliases.size()) +
                 " column aliases were given",
             ref.pos);
    }
    const std::string alias = Lower(ref.alias);
    for (idx_t i = 0; i < out.plan->ColumnCount(); i++) {
        if (i < ref.column_aliases.size())
            out.plan->names[i] = Lower(ref.column_aliases[i]);
        out.scope.columns.push_back({alias, Lower(out.plan->names[i]), out.plan->types[i], false});
    }
    return out;
}

Binder::BoundTable Binder::BindJoin(const JoinRef& ref) {
    BoundTable left = BindTableRef(*ref.left);
    BoundTable right = BindTableRef(*ref.right);

    // an alias may appear only once among the relations of one FROM clause
    for (const ScopeColumn& r : right.scope.columns) {
        if (r.table.empty())
            continue;
        for (const ScopeColumn& l : left.scope.columns) {
            if (l.table == r.table) {
                Fail(ErrorCode::Binder, "duplicate table alias \"" + r.table + "\" in FROM clause",
                     ref.right->pos);
            }
        }
    }

    auto join = std::make_unique<LogicalJoin>();
    join->join_type = ref.type;
    BoundTable out;
    out.scope.columns = left.scope.columns;
    out.scope.columns.insert(out.scope.columns.end(), right.scope.columns.begin(),
                             right.scope.columns.end());
    join->names = left.plan->names;
    join->names.insert(join->names.end(), right.plan->names.begin(), right.plan->names.end());
    join->types = left.plan->types;
    join->types.insert(join->types.end(), right.plan->types.begin(), right.plan->types.end());
    const idx_t left_width = left.plan->ColumnCount();

    if (ref.condition) {
        ExprContext ctx{&out.scope, false};
        BoundExprPtr cond = BindExpr(*ref.condition, ctx);
        if (!(cond->type.id() == TypeId::Boolean)) {
            Fail(ErrorCode::Type, "JOIN condition must be BOOLEAN but is " + cond->type.ToString(),
                 ref.condition->pos);
        }
        join->condition = std::move(cond);
    } else if (!ref.using_columns.empty()) {
        if (ref.type == JoinType::Right || ref.type == JoinType::Full) {
            Fail(ErrorCode::NotImplemented, "USING with RIGHT/FULL joins is not supported yet",
                 ref.pos);
        }
        BoundExprPtr all;
        for (const std::string& raw : ref.using_columns) {
            const std::string name = Lower(raw);
            idx_t lo = 0, ro = 0;
            if (ResolveColumn(left.scope, "", name, &lo) != Lookup::Found ||
                ResolveColumn(right.scope, "", name, &ro) != Lookup::Found) {
                Fail(ErrorCode::Binder,
                     "USING column \"" + name +
                         "\" must appear exactly once on each side of the join",
                     ref.pos);
            }
            const auto qualified = [&](const ScopeColumn& c) {
                return c.table.empty() ? c.name : c.table + "." + c.name;
            };
            BoundExprPtr l = BoundExpr::ColumnRef(lo, left.scope.columns[lo].type,
                                                  qualified(left.scope.columns[lo]));
            BoundExprPtr r = BoundExpr::ColumnRef(left_width + ro, right.scope.columns[ro].type,
                                                  qualified(right.scope.columns[ro]));
            if (l->type != r->type) {
                auto common = CommonSuperType(l->type, r->type);
                if (!common) {
                    Fail(ErrorCode::Type,
                         "USING column \"" + name + "\" has incompatible types " +
                             l->type.ToString() + " and " + r->type.ToString(),
                         ref.pos);
                }
                l = BoundExpr::Cast(std::move(l), *common);
                r = BoundExpr::Cast(std::move(r), *common);
            }
            BoundExprPtr eq = BoundExpr::Binary(OperatorKind::Eq, std::move(l), std::move(r),
                                                LogicalType::Boolean());
            all = all ? BoundExpr::Binary(OperatorKind::And, std::move(all), std::move(eq),
                                          LogicalType::Boolean())
                      : std::move(eq);
            out.scope.columns[left_width + ro].hidden = true; // `*` and bare names see the left one
        }
        join->condition = std::move(all);
    }
    join->children.push_back(std::move(left.plan));
    join->children.push_back(std::move(right.plan));
    out.plan = std::move(join);
    return out;
}

// -------------------------------------------------------------------------------- SELECT

namespace {

struct SelectItemBound {
    BoundExprPtr expr;
    std::string name;
    size_t pos; // where the item starts in the SQL text (for error messages)
};

// LIMIT / OFFSET: a constant integer expression. Returns nullopt for NULL.
std::optional<int64_t> ConstantInteger(const BoundExpr& e, const char* what, size_t pos) {
    if (e.kind != BoundKind::Constant) {
        Fail(ErrorCode::Binder, std::string(what) + " must be a constant", pos);
    }
    if (e.value->IsNull())
        return std::nullopt;
    if (!e.type.IsIntegral()) {
        Fail(ErrorCode::Type, std::string(what) + " must be an integer but is " + e.type.ToString(),
             pos);
    }
    const int64_t v = e.type.id() == TypeId::Integer ? e.value->GetInteger() : e.value->GetBigInt();
    if (v < 0)
        Fail(ErrorCode::Binder, std::string(what) + " must not be negative", pos);
    return v;
}

} // namespace

LogicalPtr Binder::BindSelect(const SelectStatement& sel) {
    // ---- FROM --------------------------------------------------------------------------
    BoundTable from;
    if (sel.from) {
        from = BindTableRef(*sel.from);
    } else {
        auto values = std::make_unique<LogicalValues>();
        values->rows.emplace_back(); // one row, no columns
        from.plan = std::move(values);
    }
    LogicalPtr plan = std::move(from.plan);
    const Scope& scope = from.scope;
    const ExprContext plain{&scope, false};
    const ExprContext with_aggs{&scope, true};

    // ---- WHERE -------------------------------------------------------------------------
    if (sel.where) {
        BoundExprPtr pred = BindExpr(*sel.where, plain);
        if (pred->type.id() != TypeId::Boolean) {
            Fail(ErrorCode::Type, "WHERE clause must be BOOLEAN but is " + pred->type.ToString(),
                 sel.where->pos);
        }
        auto filter = std::make_unique<LogicalFilter>();
        filter->names = plan->names;
        filter->types = plan->types;
        filter->predicate = std::move(pred);
        filter->children.push_back(std::move(plan));
        plan = std::move(filter);
    }

    // ---- select list -------------------------------------------------------------------
    std::vector<SelectItemBound> items;
    for (const SelectItem& si : sel.items) {
        if (si.expr->kind == ExprKind::Star) {
            const std::string qualifier = Lower(static_cast<const StarExpr&>(*si.expr).table);
            bool any = false;
            for (idx_t i = 0; i < scope.columns.size(); i++) {
                const ScopeColumn& c = scope.columns[i];
                if (c.hidden || (!qualifier.empty() && c.table != qualifier))
                    continue;
                items.push_back({BoundExpr::ColumnRef(i, c.type, c.name), c.name, si.expr->pos});
                any = true;
            }
            if (!any) {
                Fail(ErrorCode::Binder,
                     qualifier.empty()
                         ? "SELECT * requires a FROM clause"
                         : "Referenced table \"" + qualifier + "\" not found in FROM clause",
                     si.expr->pos);
            }
            continue;
        }
        items.push_back({BindExpr(*si.expr, with_aggs),
                         si.alias.empty() ? DisplayName(*si.expr) : Lower(si.alias), si.expr->pos});
    }
    if (items.empty())
        Fail(ErrorCode::Binder, "SELECT needs at least one column", 0);

    // ---- aggregation -------------------------------------------------------------------
    BoundExprPtr having;
    if (sel.having) {
        having = BindExpr(*sel.having, with_aggs);
        if (having->type.id() != TypeId::Boolean) {
            Fail(ErrorCode::Type, "HAVING clause must be BOOLEAN but is " + having->type.ToString(),
                 sel.having->pos);
        }
    }
    bool is_aggregate = !sel.group_by.empty() || having != nullptr;
    for (const auto& it : items)
        is_aggregate |= it.expr->ContainsAggregate();

    // group-by items, plus the machinery that rewrites post-aggregate expressions
    std::vector<BoundExprPtr> groups;
    std::vector<BoundExprPtr> aggregates;
    auto rewrite = [&](auto&& self, BoundExprPtr e, size_t pos) -> BoundExprPtr {
        for (idx_t g = 0; g < groups.size(); g++) {
            if (e->Equals(*groups[g])) {
                return BoundExpr::ColumnRef(g, e->type, groups[g]->ToString());
            }
        }
        if (e->kind == BoundKind::Aggregate) {
            for (const auto& arg : e->children) {
                if (arg->ContainsAggregate()) {
                    Fail(ErrorCode::Binder, "aggregate function calls cannot be nested", pos);
                }
            }
            for (idx_t a = 0; a < aggregates.size(); a++) {
                if (e->Equals(*aggregates[a])) {
                    return BoundExpr::ColumnRef(groups.size() + a, e->type,
                                                aggregates[a]->ToString());
                }
            }
            aggregates.push_back(e->Clone());
            return BoundExpr::ColumnRef(groups.size() + aggregates.size() - 1, e->type,
                                        e->ToString());
        }
        if (e->kind == BoundKind::ColumnRef) {
            Fail(ErrorCode::Binder,
                 "column \"" + e->name +
                     "\" must appear in the GROUP BY clause or be used in an aggregate function",
                 pos);
        }
        for (auto& child : e->children)
            child = self(self, std::move(child), pos);
        return e;
    };

    if (is_aggregate) {
        for (const ExprPtr& g : sel.group_by) {
            BoundExprPtr bound;
            if (g->kind == ExprKind::Constant &&
                static_cast<const ConstantExpr&>(*g).literal == LiteralKind::Integer) {
                const Value& v = static_cast<const ConstantExpr&>(*g).value;
                const int64_t n = v.type().id() == TypeId::Integer ? v.GetInteger() : v.GetBigInt();
                if (n < 1 || n > static_cast<int64_t>(items.size())) {
                    Fail(ErrorCode::Binder,
                         "GROUP BY position " + std::to_string(n) + " is not in the select list",
                         g->pos);
                }
                bound = items[static_cast<size_t>(n - 1)].expr->Clone();
            } else {
                bool via_alias = false;
                if (g->kind == ExprKind::ColumnRef &&
                    static_cast<const ColumnRefExpr&>(*g).table.empty()) {
                    // a bare name that is not an input column may name a select-list alias
                    const std::string name = Lower(static_cast<const ColumnRefExpr&>(*g).column);
                    idx_t unused = 0;
                    if (ResolveColumn(scope, "", name, &unused) == Lookup::NotFound) {
                        for (const auto& it : items) {
                            if (it.name == name) {
                                bound = it.expr->Clone();
                                via_alias = true;
                                break;
                            }
                        }
                    }
                }
                if (!via_alias)
                    bound = BindExpr(*g, plain);
            }
            if (bound->ContainsAggregate()) {
                Fail(ErrorCode::Binder, "aggregate functions are not allowed in GROUP BY", g->pos);
            }
            bool duplicate = false;
            for (const auto& existing : groups)
                duplicate |= existing->Equals(*bound);
            if (!duplicate)
                groups.push_back(std::move(bound));
        }
        // Pass 1 registers every aggregate (items, then HAVING) so the aggregate operator's
        // output is known before the projection above it is built.
        for (auto& it : items)
            it.expr = rewrite(rewrite, std::move(it.expr), it.pos);
        if (having)
            having = rewrite(rewrite, std::move(having), sel.having->pos);
    }

    // ---- ORDER BY resolution (before building the projection: it may add hidden columns) ----
    struct ResolvedOrder {
        size_t item_index; // index into items (visible or appended hidden)
        bool descending;
        bool nulls_first;
    };
    std::vector<ResolvedOrder> order;
    const size_t visible = items.size();
    for (const OrderItem& oi : sel.order_by) {
        const ParsedExpr& oe = *oi.expr;
        const bool nulls_first = oi.nulls == NullOrder::First; // default: NULLS LAST
        size_t index = std::string::npos;
        if (oe.kind == ExprKind::Constant &&
            static_cast<const ConstantExpr&>(oe).literal == LiteralKind::Integer) {
            const Value& v = static_cast<const ConstantExpr&>(oe).value;
            const int64_t n = v.type().id() == TypeId::Integer ? v.GetInteger() : v.GetBigInt();
            if (n < 1 || n > static_cast<int64_t>(visible)) {
                Fail(ErrorCode::Binder,
                     "ORDER BY position " + std::to_string(n) + " is not in the select list",
                     oe.pos);
            }
            index = static_cast<size_t>(n - 1);
        } else if (oe.kind == ExprKind::ColumnRef &&
                   static_cast<const ColumnRefExpr&>(oe).table.empty()) {
            const std::string name = Lower(static_cast<const ColumnRefExpr&>(oe).column);
            size_t matches = 0;
            for (size_t i = 0; i < visible; i++) {
                if (items[i].name == name) {
                    index = i;
                    matches++;
                }
            }
            if (matches > 1) {
                Fail(ErrorCode::Binder, "ORDER BY \"" + name + "\" is ambiguous", oe.pos);
            }
            if (matches == 0)
                index = std::string::npos;
        }
        if (index == std::string::npos) {
            BoundExprPtr bound = BindExpr(oe, is_aggregate ? with_aggs : plain);
            if (is_aggregate) {
                // re-express against the aggregate's output; new aggregates are registered
                bound = rewrite(rewrite, std::move(bound), oe.pos);
            }
            for (size_t i = 0; i < items.size(); i++) {
                if (items[i].expr->Equals(*bound))
                    index = i;
            }
            if (index == std::string::npos) {
                if (sel.distinct) {
                    Fail(ErrorCode::Binder,
                         "for SELECT DISTINCT, ORDER BY expressions must appear in the select list",
                         oe.pos);
                }
                const std::string name = DisplayName(oe);
                items.push_back({std::move(bound), name, oe.pos});
                index = items.size() - 1;
            }
        }
        order.push_back({index, oi.descending, nulls_first});
    }

    // ---- build the aggregate operator (every aggregate is now registered: items, HAVING, ORDER
    // BY) ----
    if (is_aggregate) {
        auto agg = std::make_unique<LogicalAggregate>();
        for (auto& g : groups) {
            agg->names.push_back(g->ToString());
            agg->types.push_back(g->type);
        }
        for (auto& a : aggregates) {
            agg->names.push_back(a->ToString());
            agg->types.push_back(a->type);
        }
        agg->groups = std::move(groups);
        agg->aggregates = std::move(aggregates);
        agg->children.push_back(std::move(plan));
        plan = std::move(agg);
        if (having) {
            auto filter = std::make_unique<LogicalFilter>();
            filter->names = plan->names;
            filter->types = plan->types;
            filter->predicate = std::move(having);
            filter->children.push_back(std::move(plan));
            plan = std::move(filter);
        }
    }

    // ---- projection ----------------------------------------------------------------------
    auto proj = std::make_unique<LogicalProjection>();
    for (auto& it : items) {
        proj->names.push_back(it.name);
        proj->types.push_back(it.expr->type);
        proj->exprs.push_back(std::move(it.expr));
    }
    proj->children.push_back(std::move(plan));
    plan = std::move(proj);

    if (sel.distinct) {
        auto distinct = std::make_unique<LogicalDistinct>();
        distinct->names = plan->names;
        distinct->types = plan->types;
        distinct->children.push_back(std::move(plan));
        plan = std::move(distinct);
    }

    if (!order.empty()) {
        auto ord = std::make_unique<LogicalOrder>();
        ord->names = plan->names;
        ord->types = plan->types;
        for (const ResolvedOrder& r : order) {
            SortKey key;
            key.expr = BoundExpr::ColumnRef(r.item_index, plan->types[r.item_index],
                                            plan->names[r.item_index]);
            key.descending = r.descending;
            key.nulls_first = r.nulls_first;
            ord->keys.push_back(std::move(key));
        }
        ord->children.push_back(std::move(plan));
        plan = std::move(ord);
    }

    // drop the hidden ORDER BY columns again
    if (plan->ColumnCount() > visible) {
        auto strip = std::make_unique<LogicalProjection>();
        for (size_t i = 0; i < visible; i++) {
            strip->names.push_back(plan->names[i]);
            strip->types.push_back(plan->types[i]);
            strip->exprs.push_back(BoundExpr::ColumnRef(i, plan->types[i], plan->names[i]));
        }
        strip->children.push_back(std::move(plan));
        plan = std::move(strip);
    }

    // ---- LIMIT / OFFSET --------------------------------------------------------------------
    if (sel.limit || sel.offset) {
        auto lim = std::make_unique<LogicalLimit>();
        lim->names = plan->names;
        lim->types = plan->types;
        Scope empty;
        const ExprContext ctx{&empty, false};
        if (sel.limit)
            lim->limit = ConstantInteger(*BindExpr(*sel.limit, ctx), "LIMIT", sel.limit->pos);
        if (sel.offset) {
            lim->offset =
                ConstantInteger(*BindExpr(*sel.offset, ctx), "OFFSET", sel.offset->pos).value_or(0);
        }
        lim->children.push_back(std::move(plan));
        plan = std::move(lim);
    }
    return plan;
}

} // namespace cdb
