#include "planner/optimizer.h"

#include "planner/expr_util.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>

namespace cdb {

namespace {

using Conjuncts = std::vector<BoundExprPtr>;

// ---------------------------------------------------------------------------------- helpers

// Splits `e` into conjuncts and, for every disjunction among them, pulls out the conjuncts that
// occur in all of its branches:  (a AND x) OR (a AND y)  ->  a AND (x OR y).  This is what exposes
// the equi-join key in predicates like TPC-H Q19, where every branch repeats
// `p_partkey = l_partkey`; without it the join would be a nested loop over the cross product.
void SplitAndFactor(BoundExprPtr e, Conjuncts& out) {
    Conjuncts parts;
    SplitConjuncts(std::move(e), parts);
    for (auto& part : parts) {
        if (part->kind != BoundKind::Operator || part->op != OperatorKind::Or) {
            out.push_back(std::move(part));
            continue;
        }
        std::vector<BoundExprPtr> branches; // the OR chain, flattened
        std::vector<BoundExprPtr> stack;
        stack.push_back(std::move(part));
        while (!stack.empty()) {
            BoundExprPtr cur = std::move(stack.back());
            stack.pop_back();
            if (cur->kind == BoundKind::Operator && cur->op == OperatorKind::Or) {
                stack.push_back(std::move(cur->children[1]));
                stack.push_back(std::move(cur->children[0]));
            } else {
                branches.push_back(std::move(cur));
            }
        }
        std::vector<Conjuncts> branch_parts(branches.size());
        for (size_t b = 0; b < branches.size(); b++) {
            SplitConjuncts(std::move(branches[b]), branch_parts[b]);
        }
        Conjuncts common;
        for (const auto& candidate : branch_parts[0]) {
            bool in_all = true;
            for (size_t b = 1; b < branch_parts.size() && in_all; b++) {
                in_all = std::any_of(branch_parts[b].begin(), branch_parts[b].end(),
                                     [&](const BoundExprPtr& x) { return x->Equals(*candidate); });
            }
            if (in_all) {
                common.push_back(candidate->Clone());
            }
        }
        BoundExprPtr rest; // OR of what remains of each branch; null means "TRUE"
        bool any_empty = false;
        for (auto& bp : branch_parts) {
            Conjuncts remaining;
            for (auto& x : bp) {
                const bool is_common =
                    std::any_of(common.begin(), common.end(),
                                [&](const BoundExprPtr& c) { return c->Equals(*x); });
                if (!is_common) {
                    remaining.push_back(std::move(x));
                }
            }
            if (remaining.empty()) {
                any_empty = true; // this branch is exactly the common part: the OR is implied
                continue;
            }
            BoundExprPtr branch = AndAll(std::move(remaining));
            rest = rest ? BoundExpr::Binary(OperatorKind::Or, std::move(rest), std::move(branch),
                                            LogicalType::Boolean())
                        : std::move(branch);
        }
        for (auto& c : common) {
            out.push_back(std::move(c));
        }
        if (!any_empty && rest) {
            out.push_back(std::move(rest));
        }
    }
}

LogicalPtr Wrap(LogicalPtr op, Conjuncts conjuncts) {
    if (conjuncts.empty()) {
        return op;
    }
    auto filter = std::make_unique<LogicalFilter>();
    filter->names = op->names;
    filter->types = op->types;
    filter->predicate = AndAll(std::move(conjuncts));
    filter->children.push_back(std::move(op));
    return filter;
}

std::optional<CompareOp> ToCompareOp(OperatorKind op) {
    switch (op) {
    case OperatorKind::Eq:
        return CompareOp::Eq;
    case OperatorKind::Ne:
        return CompareOp::Ne;
    case OperatorKind::Lt:
        return CompareOp::Lt;
    case OperatorKind::Le:
        return CompareOp::Le;
    case OperatorKind::Gt:
        return CompareOp::Gt;
    case OperatorKind::Ge:
        return CompareOp::Ge;
    default:
        return std::nullopt;
    }
}

CompareOp Flip(CompareOp op) {
    switch (op) {
    case CompareOp::Lt:
        return CompareOp::Gt;
    case CompareOp::Le:
        return CompareOp::Ge;
    case CompareOp::Gt:
        return CompareOp::Lt;
    case CompareOp::Ge:
        return CompareOp::Le;
    default:
        return op;
    }
}

// `column <op> constant` (either order, no casts in between) as a zone-map hint for a scan.
std::optional<TableFilter> AsTableFilter(const BoundExpr& e, const LogicalGet& get) {
    if (e.kind != BoundKind::Operator || e.children.size() != 2) {
        return std::nullopt;
    }
    const auto cmp = ToCompareOp(e.op);
    if (!cmp) {
        return std::nullopt;
    }
    const BoundExpr& a = *e.children[0];
    const BoundExpr& b = *e.children[1];
    const BoundExpr* col = nullptr;
    const BoundExpr* konst = nullptr;
    CompareOp op = *cmp;
    if (a.kind == BoundKind::ColumnRef && b.kind == BoundKind::Constant) {
        col = &a;
        konst = &b;
    } else if (b.kind == BoundKind::ColumnRef && a.kind == BoundKind::Constant) {
        col = &b;
        konst = &a;
        op = Flip(op);
    } else {
        return std::nullopt;
    }
    if (konst->value->IsNull() || konst->type != col->type) {
        return std::nullopt;
    }
    return TableFilter{get.column_ids.at(col->ordinal), op, *konst->value};
}

// Rough output cardinality, from table sizes and fixed selectivities. Only the relative sizes of
// join inputs matter.
double Selectivity(const BoundExpr& e) {
    if (e.kind == BoundKind::Operator) {
        if (e.op == OperatorKind::Eq) {
            return 0.1;
        }
        if (e.op == OperatorKind::Lt || e.op == OperatorKind::Le || e.op == OperatorKind::Gt ||
            e.op == OperatorKind::Ge) {
            return 0.33;
        }
        if (e.op == OperatorKind::And) {
            return Selectivity(*e.children[0]) * Selectivity(*e.children[1]);
        }
    }
    if (e.kind == BoundKind::InList) {
        return std::min(0.5, 0.1 * static_cast<double>(e.children.size() - 1));
    }
    if (e.kind == BoundKind::Function && e.function == FunctionId::Like) {
        return 0.2;
    }
    return 0.5;
}

double EstimateRows(const LogicalOperator& op) {
    switch (op.kind) {
    case LogicalKind::Get:
        return std::max<double>(
            1, static_cast<double>(static_cast<const LogicalGet&>(op).table->RowCount()));
    case LogicalKind::Filter:
        return std::max(1.0, EstimateRows(*op.children[0]) *
                                 Selectivity(*static_cast<const LogicalFilter&>(op).predicate));
    case LogicalKind::Aggregate: {
        const auto& agg = static_cast<const LogicalAggregate&>(op);
        return agg.groups.empty() ? 1.0 : std::max(1.0, EstimateRows(*op.children[0]) * 0.1);
    }
    case LogicalKind::Join: {
        const auto& j = static_cast<const LogicalJoin&>(op);
        const double l = EstimateRows(*op.children[0]), r = EstimateRows(*op.children[1]);
        if (j.join_type == JoinType::Left) {
            return l;
        }
        if (IsFilterJoin(j.join_type)) {
            return std::max(1.0, l * 0.5); // a semi / anti join keeps some of the left rows
        }
        return j.condition ? std::max(l, r) : l * r;
    }
    case LogicalKind::Limit: {
        const auto& lim = static_cast<const LogicalLimit&>(op);
        const double child = EstimateRows(*op.children[0]);
        return lim.limit ? std::min(child, static_cast<double>(*lim.limit)) : child;
    }
    case LogicalKind::Values:
        return std::max<double>(
            1, static_cast<double>(static_cast<const LogicalValues&>(op).rows.size()));
    default:
        return op.children.empty() ? 1.0 : EstimateRows(*op.children[0]);
    }
}

// Distinct-value estimates for join sizing. For an integer or date column read straight from a
// table (through filters and column-only projections) the zone maps bound the number of distinct
// values by the value range: min(rows, max - min + 1). That alone separates the 25 nation keys
// from the 150,000 order keys, which is what decides whether a join multiplies rows or not.
std::optional<int64_t> AsInt64(const Value& v) {
    switch (v.type().id()) {
    case TypeId::Integer:
        return v.GetInteger();
    case TypeId::BigInt:
        return v.GetBigInt();
    case TypeId::Date:
        return v.GetDate().days;
    default:
        return std::nullopt;
    }
}

double TableColumnNdv(const Table& table, idx_t table_column, double rows) {
    const auto snapshot = table.Snapshot();
    std::optional<int64_t> lo, hi;
    for (idx_t g = 0; g < snapshot->row_group_count(); g++) {
        const ColumnStats& st = snapshot->row_group(g).column(table_column).stats();
        if (!st.min || !st.max) {
            continue; // all NULL (or no usable bounds)
        }
        const auto a = AsInt64(*st.min), b = AsInt64(*st.max);
        if (!a || !b) {
            return rows; // not an integer-like column
        }
        lo = lo ? std::min(*lo, *a) : *a;
        hi = hi ? std::max(*hi, *b) : *b;
    }
    if (!lo || !hi) {
        return rows;
    }
    return std::min(rows, static_cast<double>(*hi) - static_cast<double>(*lo) + 1.0);
}

double ColumnNdv(const LogicalOperator& op, idx_t column, double rows) {
    switch (op.kind) {
    case LogicalKind::Filter:
        return std::min(rows, ColumnNdv(*op.children[0], column, EstimateRows(*op.children[0])));
    case LogicalKind::Get: {
        const auto& get = static_cast<const LogicalGet&>(op);
        const double base = static_cast<double>(get.table->RowCount());
        return std::min(rows,
                        TableColumnNdv(*get.table, get.column_ids.at(column), std::max(1.0, base)));
    }
    case LogicalKind::Projection: {
        const auto& p = static_cast<const LogicalProjection&>(op);
        if (p.exprs.at(column)->kind == BoundKind::ColumnRef) {
            return std::min(rows, ColumnNdv(*op.children[0], p.exprs[column]->ordinal,
                                            EstimateRows(*op.children[0])));
        }
        return rows;
    }
    default:
        return rows;
    }
}

LogicalPtr Push(LogicalPtr op, Conjuncts conjuncts);

// ---------------------------------------------------------------------------------- join trees

struct FlatJoin {
    std::vector<LogicalPtr> leaves;
    std::vector<idx_t> base; // first column of each leaf in the flattened (original) layout
    Conjuncts conjuncts;     // join conditions, over the flattened layout
};

void Flatten(LogicalPtr op, idx_t start, FlatJoin& flat) {
    if (op->kind == LogicalKind::Join) {
        auto& join = static_cast<LogicalJoin&>(*op);
        if (join.join_type == JoinType::Inner || join.join_type == JoinType::Cross) {
            const idx_t left_width = join.children[0]->ColumnCount();
            if (join.condition) {
                Conjuncts parts;
                SplitAndFactor(std::move(join.condition), parts);
                for (auto& p : parts) {
                    flat.conjuncts.push_back(
                        RemapColumns(*p, [start](idx_t o) { return o + start; }));
                }
            }
            LogicalPtr left = std::move(join.children[0]);
            LogicalPtr right = std::move(join.children[1]);
            Flatten(std::move(left), start, flat);
            Flatten(std::move(right), start + left_width, flat);
            return;
        }
    }
    flat.base.push_back(start);
    flat.leaves.push_back(std::move(op));
}

LogicalPtr PushInnerJoinTree(LogicalPtr tree, Conjuncts incoming) {
    FlatJoin flat;
    Flatten(std::move(tree), 0, flat);
    for (auto& c : incoming) {
        flat.conjuncts.push_back(std::move(c));
    }
    const size_t n = flat.leaves.size();
    const auto leaf_of = [&](idx_t global) {
        size_t i =
            static_cast<size_t>(std::upper_bound(flat.base.begin(), flat.base.end(), global) -
                                flat.base.begin()) -
            1;
        return i;
    };
    // The original output layout, to restore if the leaves get reordered.
    std::vector<std::string> orig_names;
    std::vector<LogicalType> orig_types;
    std::vector<idx_t> widths(n);
    for (size_t i = 0; i < n; i++) {
        widths[i] = flat.leaves[i]->ColumnCount();
        orig_names.insert(orig_names.end(), flat.leaves[i]->names.begin(),
                          flat.leaves[i]->names.end());
        orig_types.insert(orig_types.end(), flat.leaves[i]->types.begin(),
                          flat.leaves[i]->types.end());
    }

    // Which leaves does each conjunct touch?
    const bool can_mask = n <= 60;
    std::vector<uint64_t> mask(flat.conjuncts.size(), 0);
    std::vector<Conjuncts> per_leaf(n);
    std::vector<bool> consumed(flat.conjuncts.size(), false);
    for (size_t c = 0; c < flat.conjuncts.size(); c++) {
        std::set<idx_t> refs;
        CollectColumnRefs(*flat.conjuncts[c], refs);
        std::set<size_t> leaves;
        for (const idx_t g : refs) {
            leaves.insert(leaf_of(g));
        }
        if (can_mask) {
            for (const size_t l : leaves) {
                mask[c] |= uint64_t{1} << l;
            }
        }
        if (leaves.size() == 1) { // single-relation predicate: filter that leaf before joining
            const size_t l = *leaves.begin();
            const idx_t b = flat.base[l];
            per_leaf[l].push_back(RemapColumns(*flat.conjuncts[c], [b](idx_t g) { return g - b; }));
            consumed[c] = true;
        }
    }
    for (size_t i = 0; i < n; i++) {
        flat.leaves[i] = Push(std::move(flat.leaves[i]), std::move(per_leaf[i]));
    }
    if (n == 1) {
        Conjuncts rest;
        for (size_t c = 0; c < flat.conjuncts.size(); c++) {
            if (!consumed[c]) {
                rest.push_back(std::move(flat.conjuncts[c]));
            }
        }
        return Wrap(std::move(flat.leaves[0]), std::move(rest));
    }

    // ---- choose the join order ------------------------------------------------------------
    std::vector<double> est(n);
    for (size_t i = 0; i < n; i++) {
        est[i] = EstimateRows(*flat.leaves[i]);
    }
    std::vector<size_t> order;
    if (!can_mask) {
        for (size_t i = 0; i < n; i++) {
            order.push_back(i);
        }
    } else {
        uint64_t chosen = 0;
        size_t start = 0;
        for (size_t i = 1; i < n; i++) {
            if (est[i] > est[start]) {
                start = i;
            }
        }
        order.push_back(start);
        chosen |= uint64_t{1} << start;
        double current_rows = est[start];
        while (order.size() < n) {
            // Next relation: among those connected to the chosen ones by some predicate, the one
            // giving the smallest estimated intermediate result (rows x rows x selectivity, with an
            // equality's selectivity 1 / max(distinct values of its two columns)); relations with
            // no connecting predicate (a cross product) only when nothing else is left.
            bool best_connected = false;
            double best_result = 0;
            size_t best = n;
            for (size_t l = 0; l < n; l++) {
                if (chosen >> l & 1) {
                    continue;
                }
                double selectivity = 1.0;
                double key_combinations = 1.0; // product of the equalities' distinct-value counts
                bool has_equality = false;
                bool connected = false;
                for (size_t c = 0; c < flat.conjuncts.size(); c++) {
                    if (consumed[c] || !(mask[c] >> l & 1) || std::popcount(mask[c]) < 2 ||
                        (mask[c] & ~(chosen | (uint64_t{1} << l))) != 0) {
                        continue;
                    }
                    connected = true;
                    const BoundExpr& e = *flat.conjuncts[c];
                    const bool plain_equality = e.kind == BoundKind::Operator &&
                                                e.op == OperatorKind::Eq &&
                                                e.children[0]->kind == BoundKind::ColumnRef &&
                                                e.children[1]->kind == BoundKind::ColumnRef;
                    if (!plain_equality) {
                        selectivity *=
                            e.kind == BoundKind::Operator && e.op == OperatorKind::Eq ? 0.1 : 0.3;
                        continue;
                    }
                    double max_ndv = 1;
                    for (const auto& side : e.children) {
                        const size_t leaf = leaf_of(side->ordinal);
                        const double rows = (chosen >> leaf & 1) ? current_rows : est[leaf];
                        max_ndv =
                            std::max(max_ndv, ColumnNdv(*flat.leaves[leaf],
                                                        side->ordinal - flat.base[leaf], rows));
                    }
                    key_combinations *= max_ndv;
                    has_equality = true;
                }
                if (has_equality) {
                    // k equalities form a composite key: its distinct combinations cannot exceed
                    // the larger input's row count (so (partkey, suppkey) against partsupp is ~ one
                    // match per row, not 1 / (ndv1 * ndv2) of them).
                    selectivity /=
                        std::max(1.0, std::min(key_combinations, std::max(current_rows, est[l])));
                }
                const double result = std::max(1.0, current_rows * est[l] * selectivity);
                if (best == n || (connected && !best_connected) ||
                    (connected == best_connected &&
                     (result < best_result || (result == best_result && est[l] < est[best])))) {
                    best = l;
                    best_connected = connected;
                    best_result = result;
                }
            }
            order.push_back(best);
            chosen |= uint64_t{1} << best;
            current_rows = best_result;
        }
    }

    // ---- rebuild the tree left-deep in that order ------------------------------------------
    std::vector<idx_t> layout_base(n, 0);
    const size_t first = order[0];
    LogicalPtr current = std::move(flat.leaves[first]);
    idx_t width_so_far = widths[first];
    layout_base[first] = 0;
    uint64_t chosen = can_mask ? uint64_t{1} << first : 0;
    std::vector<bool> applied = consumed;
    for (size_t k = 1; k < n; k++) {
        const size_t l = order[k];
        layout_base[l] = width_so_far;
        if (can_mask) {
            chosen |= uint64_t{1} << l;
        }
        auto join = std::make_unique<LogicalJoin>();
        join->names = current->names;
        join->names.insert(join->names.end(), flat.leaves[l]->names.begin(),
                           flat.leaves[l]->names.end());
        join->types = current->types;
        join->types.insert(join->types.end(), flat.leaves[l]->types.begin(),
                           flat.leaves[l]->types.end());
        Conjuncts cond;
        for (size_t c = 0; c < flat.conjuncts.size(); c++) {
            const bool covered = can_mask ? (mask[c] & ~chosen) == 0 : k + 1 == n;
            if (applied[c] || !covered) {
                continue;
            }
            applied[c] = true;
            cond.push_back(RemapColumns(*flat.conjuncts[c], [&](idx_t g) {
                const size_t leaf = leaf_of(g);
                return layout_base[leaf] + (g - flat.base[leaf]);
            }));
        }
        join->condition = AndAll(std::move(cond));
        join->join_type = join->condition ? JoinType::Inner : JoinType::Cross;
        join->children.push_back(std::move(current));
        join->children.push_back(std::move(flat.leaves[l]));
        current = std::move(join);
        width_so_far += widths[l];
    }
    bool identity = true;
    for (size_t k = 0; k < n; k++) {
        identity = identity && order[k] == k;
    }
    if (identity) {
        return current;
    }
    auto proj = std::make_unique<LogicalProjection>();
    proj->names = orig_names;
    proj->types = orig_types;
    for (size_t i = 0; i < n; i++) {
        for (idx_t c = 0; c < widths[i]; c++) {
            const idx_t original = flat.base[i] + c;
            proj->exprs.push_back(BoundExpr::ColumnRef(layout_base[i] + c, orig_types[original],
                                                       orig_names[original]));
        }
    }
    proj->children.push_back(std::move(current));
    return proj;
}

// Semi / anti joins (unnested subqueries). Their output is the left side, so a predicate above them
// refers to left columns only and moves to the left child; a condition part that only mentions the
// right side filters the subquery's rows before the join. And the join itself moves down: if its
// condition needs only the columns of one input of an inner join below it, it is applied to that
// input first (the semi join then shrinks the rows before they are joined further, instead of
// after).
LogicalPtr PushFilterJoin(LogicalPtr op, Conjuncts conjuncts) {
    auto& join = static_cast<LogicalJoin&>(*op);
    const idx_t lw = join.children[0]->ColumnCount();
    LogicalOperator& left = *join.children[0];
    if (left.kind == LogicalKind::Join && join.condition) {
        auto& inner = static_cast<LogicalJoin&>(left);
        if (inner.join_type == JoinType::Inner || inner.join_type == JoinType::Cross) {
            const idx_t a_width = inner.children[0]->ColumnCount();
            std::set<idx_t> refs;
            CollectColumnRefs(*join.condition, refs);
            std::set<idx_t> left_refs;
            for (const idx_t r : refs) {
                if (r < lw) {
                    left_refs.insert(r);
                }
            }
            const bool in_a = !left_refs.empty() && *left_refs.rbegin() < a_width;
            const bool in_b = !left_refs.empty() && *left_refs.begin() >= a_width;
            if (in_a || in_b) {
                // Semi(A x B, S) -> Semi(A, S) x B  or  A x Semi(B, S); the inner join's own
                // condition and everything above keep their meaning (the output columns are
                // unchanged).
                const idx_t b_width = lw - a_width;
                LogicalPtr a = std::move(inner.children[0]);
                LogicalPtr b = std::move(inner.children[1]);
                LogicalPtr subquery = std::move(join.children[1]);
                auto filter = std::make_unique<LogicalJoin>();
                filter->join_type = join.join_type;
                if (in_a) {
                    // A's columns keep their ordinals; the subquery's columns now follow A alone
                    filter->condition = RemapColumns(*join.condition, [lw, b_width](idx_t o) {
                        return o < lw ? o : o - b_width;
                    });
                    filter->names = a->names;
                    filter->types = a->types;
                    filter->children.push_back(std::move(a));
                    filter->children.push_back(std::move(subquery));
                    inner.children[0] = std::move(filter);
                    inner.children[1] = std::move(b);
                } else {
                    // B's columns start at a_width in the old layout and at 0 in the new; the
                    // subquery's columns follow the left side in both
                    filter->condition =
                        RemapColumns(*join.condition, [a_width](idx_t o) { return o - a_width; });
                    filter->names = b->names;
                    filter->types = b->types;
                    filter->children.push_back(std::move(b));
                    filter->children.push_back(std::move(subquery));
                    inner.children[0] = std::move(a);
                    inner.children[1] = std::move(filter);
                }
                // `inner` is the new top: its own condition is unchanged (it only mentions A's and
                // B's columns, in the same positions)
                LogicalPtr top = std::move(join.children[0]);
                return Push(std::move(top), std::move(conjuncts));
            }
        }
    }
    Conjuncts to_left, keep, to_right;
    for (auto& c : conjuncts) {
        to_left.push_back(std::move(c)); // above a filter join only left columns exist
    }
    if (join.condition) {
        Conjuncts parts;
        SplitAndFactor(std::move(join.condition), parts);
        for (auto& p : parts) {
            std::set<idx_t> refs;
            CollectColumnRefs(*p, refs);
            const bool left_only = !refs.empty() && *refs.rbegin() < lw;
            const bool right_only = !refs.empty() && *refs.begin() >= lw;
            if (right_only) {
                to_right.push_back(RemapColumns(*p, [lw](idx_t o) { return o - lw; }));
            } else if (left_only && join.join_type == JoinType::Semi) {
                to_left.push_back(
                    std::move(p)); // EXISTS (... AND f(left)) = f(left) AND EXISTS (...)
            } else {
                keep.push_back(std::move(p)); // for anti joins f(left) cannot leave the condition
            }
        }
    }
    join.condition = AndAll(std::move(keep));
    join.children[0] = Push(std::move(join.children[0]), std::move(to_left));
    join.children[1] = Push(std::move(join.children[1]), std::move(to_right));
    return op;
}

LogicalPtr PushJoin(LogicalPtr op, Conjuncts conjuncts) {
    auto& join = static_cast<LogicalJoin&>(*op);
    if (join.join_type == JoinType::Inner || join.join_type == JoinType::Cross) {
        return PushInnerJoinTree(std::move(op), std::move(conjuncts));
    }
    if (IsFilterJoin(join.join_type)) {
        return PushFilterJoin(std::move(op), std::move(conjuncts));
    }
    const idx_t lw = join.children[0]->ColumnCount();
    const bool preserve_left = join.join_type == JoinType::Left;
    const bool preserve_right = join.join_type == JoinType::Right;
    Conjuncts above, to_left, to_right;
    for (auto& c : conjuncts) {
        std::set<idx_t> refs;
        CollectColumnRefs(*c, refs);
        const bool left_only = !refs.empty() && *refs.rbegin() < lw;
        const bool right_only = !refs.empty() && *refs.begin() >= lw;
        if (left_only && preserve_left) {
            to_left.push_back(std::move(c));
        } else if (right_only && preserve_right) {
            to_right.push_back(RemapColumns(*c, [lw](idx_t o) { return o - lw; }));
        } else {
            above.push_back(std::move(c)); // would change which rows the outer join pads
        }
    }
    Conjuncts keep;
    if (join.condition) {
        Conjuncts parts;
        SplitAndFactor(std::move(join.condition), parts);
        for (auto& p : parts) {
            std::set<idx_t> refs;
            CollectColumnRefs(*p, refs);
            const bool left_only = !refs.empty() && *refs.rbegin() < lw;
            const bool right_only = !refs.empty() && *refs.begin() >= lw;
            // An ON conjunct on the non-preserved side only decides which rows can match.
            if (preserve_left && right_only) {
                to_right.push_back(RemapColumns(*p, [lw](idx_t o) { return o - lw; }));
            } else if (preserve_right && left_only) {
                to_left.push_back(std::move(p));
            } else {
                keep.push_back(std::move(p));
            }
        }
    }
    join.condition = AndAll(std::move(keep));
    join.children[0] = Push(std::move(join.children[0]), std::move(to_left));
    join.children[1] = Push(std::move(join.children[1]), std::move(to_right));
    return Wrap(std::move(op), std::move(above));
}

LogicalPtr Push(LogicalPtr op, Conjuncts conjuncts) {
    switch (op->kind) {
    case LogicalKind::Filter: {
        auto& f = static_cast<LogicalFilter&>(*op);
        SplitAndFactor(std::move(f.predicate), conjuncts);
        LogicalPtr child = std::move(f.children[0]);
        return Push(std::move(child), std::move(conjuncts));
    }
    case LogicalKind::Get: {
        auto& get = static_cast<LogicalGet&>(*op);
        for (const auto& c : conjuncts) {
            if (auto tf = AsTableFilter(*c, get)) {
                get.filters.push_back(std::move(*tf));
            }
        }
        return Wrap(std::move(op), std::move(conjuncts));
    }
    case LogicalKind::Projection: {
        auto& p = static_cast<LogicalProjection&>(*op);
        Conjuncts below;
        for (auto& c : conjuncts) {
            below.push_back(SubstituteColumns(*c, p.exprs));
        }
        p.children[0] = Push(std::move(p.children[0]), std::move(below));
        return op;
    }
    case LogicalKind::Aggregate: {
        auto& agg = static_cast<LogicalAggregate&>(*op);
        Conjuncts below, above;
        for (auto& c : conjuncts) {
            std::set<idx_t> refs;
            CollectColumnRefs(*c, refs);
            if (!refs.empty() && *refs.rbegin() < agg.groups.size()) {
                below.push_back(SubstituteColumns(*c, agg.groups));
            } else {
                above.push_back(std::move(c));
            }
        }
        agg.children[0] = Push(std::move(agg.children[0]), std::move(below));
        return Wrap(std::move(op), std::move(above));
    }
    case LogicalKind::Join:
        return PushJoin(std::move(op), std::move(conjuncts));
    case LogicalKind::ScalarGuard:
        // one row whatever the child holds: nothing moves through it, but the subquery under it is
        // optimized on its own
        op->children[0] = Push(std::move(op->children[0]), {});
        return Wrap(std::move(op), std::move(conjuncts));
    case LogicalKind::Order:
    case LogicalKind::Distinct:
        op->children[0] = Push(std::move(op->children[0]), std::move(conjuncts));
        return op;
    case LogicalKind::Limit:
        if (op->children[0]->kind == LogicalKind::Projection) {
            // A projection is row-wise, so LIMIT commutes with it: LIMIT(PROJECT(x)) =
            // PROJECT(LIMIT(x)). This puts LIMIT directly above ORDER BY, where it becomes a top-N.
            LogicalPtr proj = std::move(op->children[0]);
            op->children[0] = std::move(proj->children[0]);
            op->names = op->children[0]->names;
            op->types = op->children[0]->types;
            proj->children[0] = std::move(op);
            return Push(std::move(proj), std::move(conjuncts));
        }
        op->children[0] = Push(std::move(op->children[0]), {});
        return Wrap(std::move(op), std::move(conjuncts));
    default:
        return Wrap(std::move(op), std::move(conjuncts));
    }
}

// ---------------------------------------------------------------------------------- pruning

constexpr idx_t kDropped = ~idx_t{0};
using Mapping = std::vector<idx_t>; // old output ordinal -> new ordinal (kDropped if removed)

void Need(const BoundExpr& e, std::vector<bool>& required) {
    std::set<idx_t> refs;
    CollectColumnRefs(e, refs);
    for (const idx_t r : refs) {
        required.at(r) = true;
    }
}

BoundExprPtr Remap(const BoundExpr& e, const Mapping& m) {
    return RemapColumns(e, [&m](idx_t o) {
        CDB_CHECK(m.at(o) != kDropped);
        return m[o];
    });
}

Mapping Identity(idx_t n) {
    Mapping m(n);
    for (idx_t i = 0; i < n; i++) {
        m[i] = i;
    }
    return m;
}

Mapping Prune(LogicalOperator& op, const std::vector<bool>& required) {
    switch (op.kind) {
    case LogicalKind::Get: {
        auto& get = static_cast<LogicalGet&>(op);
        Mapping m(op.ColumnCount(), kDropped);
        std::vector<idx_t> ids;
        std::vector<std::string> names;
        std::vector<LogicalType> types;
        for (idx_t i = 0; i < op.ColumnCount(); i++) {
            if (required[i]) {
                m[i] = ids.size();
                ids.push_back(get.column_ids[i]);
                names.push_back(get.names[i]);
                types.push_back(get.types[i]);
            }
        }
        if (ids.empty() && op.ColumnCount() > 0) { // COUNT(*) etc.: a scan still needs a column
            m[0] = 0;
            ids.push_back(get.column_ids[0]);
            names.push_back(get.names[0]);
            types.push_back(get.types[0]);
        }
        get.column_ids = std::move(ids);
        get.names = std::move(names);
        get.types = std::move(types);
        return m;
    }
    case LogicalKind::Filter: {
        auto& f = static_cast<LogicalFilter&>(op);
        std::vector<bool> need = required;
        Need(*f.predicate, need);
        const Mapping m = Prune(*op.children[0], need);
        f.predicate = Remap(*f.predicate, m);
        f.names = op.children[0]->names;
        f.types = op.children[0]->types;
        return m;
    }
    case LogicalKind::Projection: {
        auto& p = static_cast<LogicalProjection&>(op);
        std::vector<bool> need(op.children[0]->ColumnCount(), false);
        for (idx_t i = 0; i < p.exprs.size(); i++) {
            if (required[i]) {
                Need(*p.exprs[i], need);
            }
        }
        const Mapping child = Prune(*op.children[0], need);
        Mapping m(p.exprs.size(), kDropped);
        std::vector<BoundExprPtr> exprs;
        std::vector<std::string> names;
        std::vector<LogicalType> types;
        for (idx_t i = 0; i < p.exprs.size(); i++) {
            if (required[i]) {
                m[i] = exprs.size();
                exprs.push_back(Remap(*p.exprs[i], child));
                names.push_back(p.names[i]);
                types.push_back(p.types[i]);
            }
        }
        p.exprs = std::move(exprs);
        p.names = std::move(names);
        p.types = std::move(types);
        return m;
    }
    case LogicalKind::Aggregate: {
        auto& a = static_cast<LogicalAggregate&>(op);
        std::vector<bool> need(op.children[0]->ColumnCount(), false);
        for (const auto& g : a.groups) {
            Need(*g, need);
        }
        for (const auto& x : a.aggregates) {
            Need(*x, need);
        }
        const Mapping child = Prune(*op.children[0], need);
        for (auto& g : a.groups) {
            g = Remap(*g, child);
        }
        for (auto& x : a.aggregates) {
            x = Remap(*x, child);
        }
        return Identity(op.ColumnCount());
    }
    case LogicalKind::Join: {
        auto& j = static_cast<LogicalJoin&>(op);
        const idx_t lw = op.children[0]->ColumnCount();
        if (IsFilterJoin(j.join_type)) {
            // output = the left columns; the condition may also need left and right columns
            std::vector<bool> need_left = required;
            std::vector<bool> need_right(op.children[1]->ColumnCount(), false);
            if (j.condition) {
                std::set<idx_t> refs;
                CollectColumnRefs(*j.condition, refs);
                for (const idx_t r : refs) {
                    if (r < lw) {
                        need_left.at(r) = true;
                    } else {
                        need_right.at(r - lw) = true;
                    }
                }
            }
            const Mapping lm = Prune(*op.children[0], need_left);
            const Mapping rm = Prune(*op.children[1], need_right);
            const idx_t new_lw = op.children[0]->ColumnCount();
            Mapping both(lw + rm.size(), kDropped);
            for (idx_t i = 0; i < lw; i++) {
                both[i] = lm[i];
            }
            for (idx_t i = 0; i < rm.size(); i++) {
                both[lw + i] = rm[i] == kDropped ? kDropped : new_lw + rm[i];
            }
            if (j.condition) {
                j.condition = Remap(*j.condition, both);
            }
            j.names = op.children[0]->names;
            j.types = op.children[0]->types;
            return lm;
        }
        std::vector<bool> need = required;
        if (j.condition) {
            Need(*j.condition, need);
        }
        std::vector<bool> need_left(need.begin(), need.begin() + static_cast<long>(lw));
        std::vector<bool> need_right(need.begin() + static_cast<long>(lw), need.end());
        const Mapping lm = Prune(*op.children[0], need_left);
        const Mapping rm = Prune(*op.children[1], need_right);
        const idx_t new_lw = op.children[0]->ColumnCount();
        Mapping m(op.ColumnCount(), kDropped);
        for (idx_t i = 0; i < lw; i++) {
            m[i] = lm[i];
        }
        for (idx_t i = 0; i < rm.size(); i++) {
            m[lw + i] = rm[i] == kDropped ? kDropped : new_lw + rm[i];
        }
        if (j.condition) {
            j.condition = Remap(*j.condition, m);
        }
        j.names = op.children[0]->names;
        j.names.insert(j.names.end(), op.children[1]->names.begin(), op.children[1]->names.end());
        j.types = op.children[0]->types;
        j.types.insert(j.types.end(), op.children[1]->types.begin(), op.children[1]->types.end());
        return m;
    }
    case LogicalKind::Order: {
        auto& o = static_cast<LogicalOrder&>(op);
        std::vector<bool> need = required;
        for (const SortKey& k : o.keys) {
            Need(*k.expr, need);
        }
        const Mapping m = Prune(*op.children[0], need);
        for (SortKey& k : o.keys) {
            k.expr = Remap(*k.expr, m);
        }
        o.names = op.children[0]->names;
        o.types = op.children[0]->types;
        return m;
    }
    case LogicalKind::Limit: {
        const Mapping m = Prune(*op.children[0], required);
        op.names = op.children[0]->names;
        op.types = op.children[0]->types;
        return m;
    }
    case LogicalKind::Distinct:
    case LogicalKind::ScalarGuard: {
        // DISTINCT depends on every column (and a scalar guard counts rows): nothing below may be
        // dropped.
        Prune(*op.children[0], std::vector<bool>(op.ColumnCount(), true));
        return Identity(op.ColumnCount());
    }
    default:
        return Identity(op.ColumnCount());
    }
}

} // namespace

LogicalPtr Optimize(LogicalPtr plan) {
    if (plan->kind == LogicalKind::Insert) {
        plan->children[0] = Optimize(std::move(plan->children[0]));
        return plan;
    }
    plan = Push(std::move(plan), {});
    Prune(*plan, std::vector<bool>(plan->ColumnCount(), true));
    return plan;
}

} // namespace cdb
