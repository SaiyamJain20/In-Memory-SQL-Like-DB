#include "execution/hash_aggregate.h"

#include "execution/expression_executor.h"

#include <mutex>

namespace cdb {

namespace {

struct AggGlobalState final : GlobalSinkState {
    std::mutex mutex;
    std::unique_ptr<GroupTable> table;
};

struct AggLocalState final : LocalSinkState {
    AggLocalState(std::vector<LogicalType> group_types, std::vector<AggregateSpec> specs)
        : table(std::move(group_types), std::move(specs)) {}
    GroupTable table;
    std::vector<ExpressionExecutor> group_executors;
    std::vector<ExpressionExecutor> arg_executors; // one per aggregate that has an argument
    std::vector<int> arg_column;                   // aggregate -> column of `args`, or -1
    DataChunk keys;
    DataChunk args;
};

struct AggSourceState final : GlobalSourceState {
    const GroupTable* table = nullptr;
    idx_t next = 0;
};
struct AggLocalSourceState final : LocalSourceState {};

} // namespace

PhysicalHashAggregate::PhysicalHashAggregate(std::vector<LogicalType> types,
                                             std::vector<BoundExprPtr> groups,
                                             std::vector<BoundExprPtr> aggregates)
    : PhysicalOperator(std::move(types)), groups_(std::move(groups)),
      aggregates_(std::move(aggregates)) {
    CDB_CHECK(this->types().size() == groups_.size() + aggregates_.size());
    for (const auto& a : aggregates_) {
        CDB_CHECK(a->kind == BoundKind::Aggregate);
    }
}

std::vector<LogicalType> PhysicalHashAggregate::GroupTypes() const {
    std::vector<LogicalType> t;
    for (const auto& g : groups_) {
        t.push_back(g->type);
    }
    return t;
}

std::vector<AggregateSpec> PhysicalHashAggregate::Specs() const {
    std::vector<AggregateSpec> specs;
    for (const auto& a : aggregates_) {
        AggregateSpec s;
        s.kind = a->aggregate;
        s.distinct = a->flag;
        s.arg_type = a->children.empty() ? LogicalType::Integer() : a->children[0]->type;
        specs.push_back(s);
    }
    return specs;
}

std::string PhysicalHashAggregate::Describe() const {
    std::string out = Name() + " [";
    for (size_t i = 0; i < groups_.size(); i++) {
        out += (i ? ", " : "") + groups_[i]->ToString();
    }
    out += groups_.empty() ? "" : " | ";
    for (size_t i = 0; i < aggregates_.size(); i++) {
        out += (i ? ", " : "") + aggregates_[i]->ToString();
    }
    return out + "]";
}

std::unique_ptr<GlobalSinkState> PhysicalHashAggregate::GetGlobalSinkState() {
    return std::make_unique<AggGlobalState>();
}

std::unique_ptr<LocalSinkState> PhysicalHashAggregate::GetLocalSinkState(GlobalSinkState&) {
    auto local = std::make_unique<AggLocalState>(GroupTypes(), Specs());
    for (const auto& g : groups_) {
        local->group_executors.emplace_back(*g);
    }
    std::vector<LogicalType> arg_types;
    for (const auto& a : aggregates_) {
        if (a->children.empty()) {
            local->arg_column.push_back(-1);
        } else {
            local->arg_column.push_back(static_cast<int>(arg_types.size()));
            local->arg_executors.emplace_back(*a->children[0]);
            arg_types.push_back(a->children[0]->type);
        }
    }
    local->keys.Initialize(GroupTypes());
    local->args.Initialize(arg_types);
    return local;
}

SinkResult PhysicalHashAggregate::Sink(GlobalSinkState&, LocalSinkState& state,
                                       const DataChunk& input) {
    auto& l = static_cast<AggLocalState&>(state);
    for (size_t g = 0; g < l.group_executors.size(); g++) {
        l.group_executors[g].Execute(input, l.keys.column(g));
    }
    l.keys.SetCardinality(input.size());
    for (size_t a = 0; a < l.arg_executors.size(); a++) {
        l.arg_executors[a].Execute(input, l.args.column(a));
    }
    l.args.SetCardinality(input.size());
    std::vector<const Vector*> arg_vectors;
    for (const int col : l.arg_column) {
        arg_vectors.push_back(col < 0 ? nullptr : &l.args.column(static_cast<idx_t>(col)));
    }
    l.table.Sink(l.keys, arg_vectors, input.size());
    return SinkResult::NeedMoreInput;
}

void PhysicalHashAggregate::Combine(GlobalSinkState& global, LocalSinkState& local) {
    auto& g = static_cast<AggGlobalState&>(global);
    auto& l = static_cast<AggLocalState&>(local);
    const std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.table) {
        g.table = std::make_unique<GroupTable>(std::move(l.table)); // adopt the first table
    } else {
        g.table->Combine(l.table);
    }
}

void PhysicalHashAggregate::Finalize(GlobalSinkState& global) {
    auto& g = static_cast<AggGlobalState&>(global);
    if (!g.table) { // no thread ever produced a local state
        g.table = std::make_unique<GroupTable>(GroupTypes(), Specs());
    }
}

std::unique_ptr<GlobalSourceState>
PhysicalHashAggregate::GetGlobalSourceState(GlobalSinkState* sink_state) {
    auto s = std::make_unique<AggSourceState>();
    s->table = static_cast<AggGlobalState*>(sink_state)->table.get();
    return s;
}

std::unique_ptr<LocalSourceState> PhysicalHashAggregate::GetLocalSourceState(GlobalSourceState&) {
    return std::make_unique<AggLocalSourceState>();
}

bool PhysicalHashAggregate::GetData(GlobalSourceState& global, LocalSourceState&, DataChunk& out) {
    auto& s = static_cast<AggSourceState&>(global);
    if (s.next >= s.table->GroupCount()) {
        return false;
    }
    const idx_t n = std::min<idx_t>(kVectorSize, s.table->GroupCount() - s.next);
    s.table->Scan(s.next, n, out);
    s.next += n;
    return true;
}

} // namespace cdb
