#include "execution/physical_planner.h"

#include "execution/scalar_guard.h"

#include "execution/basic_operators.h"
#include "execution/hash_aggregate.h"
#include "execution/hash_join.h"
#include "execution/sort.h"
#include "planner/expr_util.h"

#include <map>

namespace cdb {

namespace {

struct JoinSplit {
    std::vector<BoundExprPtr> left_keys, right_keys; // right keys use the right input's ordinals
    BoundExprPtr residual;                           // over left ++ right
};

// Equality conjuncts `L = R` where L only touches left columns and R only right ones become hash
// keys; everything else is the residual.
JoinSplit SplitCondition(const BoundExpr* condition, idx_t left_width) {
    JoinSplit out;
    if (!condition) {
        return out;
    }
    std::vector<BoundExprPtr> parts, rest;
    SplitConjuncts(condition->Clone(), parts);
    for (auto& p : parts) {
        bool is_key = false;
        if (p->kind == BoundKind::Operator && p->op == OperatorKind::Eq &&
            p->children[0]->type == p->children[1]->type) {
            std::set<idx_t> a, b;
            CollectColumnRefs(*p->children[0], a);
            CollectColumnRefs(*p->children[1], b);
            const bool a_left = !a.empty() && *a.rbegin() < left_width;
            const bool a_right = !a.empty() && *a.begin() >= left_width;
            const bool b_left = !b.empty() && *b.rbegin() < left_width;
            const bool b_right = !b.empty() && *b.begin() >= left_width;
            const auto shift = [left_width](idx_t o) { return o - left_width; };
            if (a_left && b_right) {
                out.left_keys.push_back(std::move(p->children[0]));
                out.right_keys.push_back(RemapColumns(*p->children[1], shift));
                is_key = true;
            } else if (a_right && b_left) {
                out.left_keys.push_back(std::move(p->children[1]));
                out.right_keys.push_back(RemapColumns(*p->children[0], shift));
                is_key = true;
            }
        }
        if (!is_key) {
            rest.push_back(std::move(p));
        }
    }
    out.residual = AndAll(std::move(rest));
    return out;
}

class Builder {
  public:
    explicit Builder(PhysicalPlan& plan) : plan_(plan) {}

    // Builds `op` into `current` (which has no sink yet); pipelines it depends on are completed
    // and recorded in plan_.pipelines first.
    void Build(const LogicalOperator& op, Pipeline& current) {
        switch (op.kind) {
        case LogicalKind::Get:
            BuildGet(static_cast<const LogicalGet&>(op), current);
            return;
        case LogicalKind::Values: {
            const auto& v = static_cast<const LogicalValues&>(op);
            std::vector<std::vector<BoundExprPtr>> rows;
            for (const auto& row : v.rows) {
                std::vector<BoundExprPtr> r;
                for (const auto& e : row) {
                    r.push_back(e->Clone());
                }
                rows.push_back(std::move(r));
            }
            current.source = &MakeFor<PhysicalValues>(op, op.types, std::move(rows));
            return;
        }
        case LogicalKind::Filter:
            Build(*op.children[0], current);
            current.operators.push_back(&MakeFor<PhysicalFilter>(
                op, op.types, static_cast<const LogicalFilter&>(op).predicate->Clone()));
            return;
        case LogicalKind::Projection: {
            Build(*op.children[0], current);
            std::vector<BoundExprPtr> exprs;
            for (const auto& e : static_cast<const LogicalProjection&>(op).exprs) {
                exprs.push_back(e->Clone());
            }
            current.operators.push_back(
                &MakeFor<PhysicalProjection>(op, op.types, std::move(exprs)));
            return;
        }
        case LogicalKind::Limit:
            BuildLimit(static_cast<const LogicalLimit&>(op), current);
            return;
        case LogicalKind::Order: {
            const auto& o = static_cast<const LogicalOrder&>(op);
            Breaker(*op.children[0], current,
                    MakeFor<PhysicalOrder>(op, op.types, CloneKeys(o.keys)));
            return;
        }
        case LogicalKind::Distinct: {
            std::vector<BoundExprPtr> groups;
            for (idx_t c = 0; c < op.ColumnCount(); c++) {
                groups.push_back(BoundExpr::ColumnRef(c, op.types[c], op.names[c]));
            }
            Breaker(*op.children[0], current,
                    MakeFor<PhysicalHashAggregate>(op, op.types, std::move(groups),
                                                   std::vector<BoundExprPtr>{}));
            return;
        }
        case LogicalKind::Aggregate: {
            const auto& a = static_cast<const LogicalAggregate&>(op);
            std::vector<BoundExprPtr> groups, aggs;
            for (const auto& g : a.groups) {
                groups.push_back(g->Clone());
            }
            for (const auto& x : a.aggregates) {
                aggs.push_back(x->Clone());
            }
            Breaker(
                *op.children[0], current,
                MakeFor<PhysicalHashAggregate>(op, op.types, std::move(groups), std::move(aggs)));
            return;
        }
        case LogicalKind::Join:
            BuildJoin(static_cast<const LogicalJoin&>(op), current);
            return;
        case LogicalKind::ScalarGuard:
            Breaker(*op.children[0], current, MakeFor<PhysicalScalarGuard>(op, op.types));
            return;
        default:
            throw Error(ErrorCode::Internal, "cannot plan operator: " + op.Describe());
        }
    }

    size_t Finish(Pipeline pipeline) {
        plan_.pipelines.push_back(std::move(pipeline));
        return plan_.pipelines.size() - 1;
    }

  private:
    // Plans an operator and remembers which logical operator it came from.
    template <class T, class... Args> T& MakeFor(const LogicalOperator& logical, Args&&... args) {
        T& op = plan_.Make<T>(std::forward<Args>(args)...);
        plan_.origins.push_back({&logical, &op});
        return op;
    }

    static std::vector<SortKey> CloneKeys(const std::vector<SortKey>& keys) {
        std::vector<SortKey> out;
        for (const SortKey& k : keys) {
            out.push_back({k.expr->Clone(), k.descending, k.nulls_first});
        }
        return out;
    }

    // `child` runs as its own pipeline ending in `breaker`; the current pipeline then starts from
    // it.
    void Breaker(const LogicalOperator& child, Pipeline& current, PhysicalOperator& breaker) {
        Pipeline p;
        Build(child, p);
        p.sink = &breaker;
        const size_t index = Finish(std::move(p));
        current.source = &breaker;
        current.dependencies.push_back(index);
    }

    void BuildGet(const LogicalGet& get, Pipeline& current) {
        std::shared_ptr<const TableSnapshot>& snap = snapshots_[get.table.get()];
        if (!snap) {
            snap = get.table->Snapshot();
        }
        current.source = &MakeFor<PhysicalTableScan>(get, get.table->name(), snap, get.column_ids,
                                                     get.filters, get.types);
    }

    void BuildLimit(const LogicalLimit& limit, Pipeline& current) {
        const LogicalOperator& child = *limit.children[0];
        if (limit.limit && child.kind == LogicalKind::Order) {
            const auto& order = static_cast<const LogicalOrder&>(child);
            PhysicalOperator& top_n = MakeFor<PhysicalTopN>(
                limit, child.types, CloneKeys(order.keys), *limit.limit, limit.offset);
            plan_.origins.push_back({&child, &top_n}); // the ORDER BY under the LIMIT is part of it
            Breaker(*child.children[0], current, top_n);
            return;
        }
        Build(child, current);
        current.operators.push_back(
            &MakeFor<PhysicalLimit>(limit, limit.types, limit.limit, limit.offset));
    }

    void BuildJoin(const LogicalJoin& join, Pipeline& current) {
        const LogicalOperator& left = *join.children[0];
        const LogicalOperator& right = *join.children[1];
        const idx_t lw = left.ColumnCount(), rw = right.ColumnCount();
        if (join.join_type == JoinType::Full) {
            throw Error(ErrorCode::NotImplemented, "FULL OUTER JOIN is not supported yet");
        }
        const bool swap = join.join_type == JoinType::Right;
        JoinSplit split = SplitCondition(join.condition.get(), lw);
        PhysicalJoinType type = PhysicalJoinType::Left;
        switch (join.join_type) {
        case JoinType::Inner:
        case JoinType::Cross:
            type = PhysicalJoinType::Inner;
            break;
        case JoinType::Semi:
            type = PhysicalJoinType::Semi;
            break;
        case JoinType::Anti:
            type = PhysicalJoinType::Anti;
            break;
        case JoinType::AntiNullAware:
            type = PhysicalJoinType::AntiNullAware;
            break;
        default:
            break; // Left / Right
        }

        const LogicalOperator& probe = swap ? right : left;
        const LogicalOperator& build = swap ? left : right;
        std::vector<LogicalType> out_types = probe.types;
        if (!IsFilterJoin(join.join_type)) { // semi and anti joins keep the probe columns only
            out_types.insert(out_types.end(), build.types.begin(), build.types.end());
        }
        BoundExprPtr residual = std::move(split.residual);
        std::vector<BoundExprPtr> probe_keys = std::move(swap ? split.right_keys : split.left_keys);
        std::vector<BoundExprPtr> build_keys = std::move(swap ? split.left_keys : split.right_keys);
        if (swap && residual) { // layout is now right ++ left
            residual =
                RemapColumns(*residual, [lw, rw](idx_t o) { return o < lw ? o + rw : o - lw; });
        }

        Build(probe, current);
        auto& op = MakeFor<PhysicalHashJoin>(join, out_types, type, probe.types, build.types,
                                             std::move(probe_keys), std::move(build_keys),
                                             std::move(residual));
        Pipeline b;
        Build(build, b);
        b.sink = &op;
        const size_t index = Finish(std::move(b));
        current.operators.push_back(&op);
        current.dependencies.push_back(index);

        if (swap) { // restore left ++ right column order
            std::vector<BoundExprPtr> exprs;
            for (idx_t i = 0; i < lw; i++) {
                exprs.push_back(BoundExpr::ColumnRef(rw + i, left.types[i], left.names[i]));
            }
            for (idx_t i = 0; i < rw; i++) {
                exprs.push_back(BoundExpr::ColumnRef(i, right.types[i], right.names[i]));
            }
            current.operators.push_back(
                &plan_.Make<PhysicalProjection>(join.types, std::move(exprs)));
        }
    }

    PhysicalPlan& plan_;
    std::map<const Table*, std::shared_ptr<const TableSnapshot>> snapshots_;
};

} // namespace

std::unique_ptr<PhysicalPlan> PlanSelect(const LogicalOperator& root) {
    auto plan = std::make_unique<PhysicalPlan>();
    Builder builder(*plan);
    Pipeline top;
    builder.Build(root, top);
    auto& collector = plan->Make<PhysicalResultCollector>(root.types);
    top.sink = &collector;
    plan->root = &collector;
    builder.Finish(std::move(top));
    return plan;
}

std::unique_ptr<PhysicalPlan> PlanInsert(const LogicalInsert& insert,
                                         PhysicalInsert::Commit commit) {
    auto plan = std::make_unique<PhysicalPlan>();
    Builder builder(*plan);
    Pipeline top;
    builder.Build(*insert.children[0], top);
    auto& sink = plan->Make<PhysicalInsert>(insert.table, std::move(commit));
    top.sink = &sink;
    plan->root = &sink;
    builder.Finish(std::move(top));
    return plan;
}

} // namespace cdb
