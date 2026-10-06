#include "planner/expr_util.h"

namespace cdb {

void CollectColumnRefs(const BoundExpr& e, std::set<idx_t>& out) {
    e.ForEach([&](const BoundExpr& x) {
        if (x.kind == BoundKind::ColumnRef) {
            out.insert(x.ordinal);
        }
    });
}

namespace {
void RemapInPlace(BoundExpr& e, const std::function<idx_t(idx_t)>& map) {
    if (e.kind == BoundKind::ColumnRef) {
        e.ordinal = map(e.ordinal);
    }
    for (auto& c : e.children) {
        RemapInPlace(*c, map);
    }
}

void SubstituteInPlace(BoundExprPtr& slot, const std::vector<BoundExprPtr>& replacement) {
    if (slot->kind == BoundKind::ColumnRef) {
        slot = replacement.at(slot->ordinal)->Clone();
        return;
    }
    for (auto& c : slot->children) {
        SubstituteInPlace(c, replacement);
    }
}
} // namespace

BoundExprPtr RemapColumns(const BoundExpr& e, const std::function<idx_t(idx_t)>& map) {
    BoundExprPtr copy = e.Clone();
    RemapInPlace(*copy, map);
    return copy;
}

BoundExprPtr SubstituteColumns(const BoundExpr& e, const std::vector<BoundExprPtr>& replacement) {
    BoundExprPtr copy = e.Clone();
    SubstituteInPlace(copy, replacement);
    return copy;
}

void SplitConjuncts(BoundExprPtr e, std::vector<BoundExprPtr>& out) {
    if (e->kind == BoundKind::Operator && e->op == OperatorKind::And) {
        SplitConjuncts(std::move(e->children[0]), out);
        SplitConjuncts(std::move(e->children[1]), out);
        return;
    }
    out.push_back(std::move(e));
}

BoundExprPtr AndAll(std::vector<BoundExprPtr> conjuncts) {
    BoundExprPtr acc;
    for (auto& c : conjuncts) {
        acc = acc ? BoundExpr::Binary(OperatorKind::And, std::move(acc), std::move(c),
                                      LogicalType::Boolean())
                  : std::move(c);
    }
    return acc;
}

} // namespace cdb
