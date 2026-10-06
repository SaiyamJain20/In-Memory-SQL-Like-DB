#include "execution/hash_aggregate.h"

#include "execution/expression_executor.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace cdb {

namespace {

// Below this many groups (summed over the threads' tables) one table is merged serially: the
// partitioning has a fixed cost that only pays off for large results.
constexpr idx_t kDefaultMinGroupsToPartition = 32768;

std::atomic<idx_t>& MinGroupsSetting() {
    static std::atomic<idx_t> groups{[] {
        const char* env = std::getenv("CDB_PARTITION_MIN_GROUPS");
        const long long v = env != nullptr ? std::atoll(env) : 0;
        return v > 0 ? static_cast<idx_t>(v) : idx_t{0};
    }()};
    return groups;
}

struct AggGlobalState final : GlobalSinkState {
    std::mutex mutex;
    std::vector<std::unique_ptr<GroupTable>> locals; // each thread's table, until Finalize
    // The result: one table, or one per hash partition (their group sets are disjoint).
    std::vector<std::unique_ptr<GroupTable>> tables;
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

// The output cut into chunk-sized ranges of groups, claimed from an atomic cursor.
struct AggSourceState final : GlobalSourceState {
    struct Range {
        const GroupTable* table;
        idx_t first, count;
    };
    std::vector<Range> ranges;
    std::atomic<size_t> next{0};
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

idx_t PhysicalHashAggregate::MinGroupsToPartition() noexcept {
    const idx_t set = MinGroupsSetting().load(std::memory_order_relaxed);
    return set != 0 ? set : kDefaultMinGroupsToPartition;
}

void PhysicalHashAggregate::SetMinGroupsToPartition(idx_t groups) noexcept {
    MinGroupsSetting().store(groups, std::memory_order_relaxed);
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
    auto table = std::make_unique<GroupTable>(std::move(l.table));
    const std::lock_guard<std::mutex> lock(g.mutex);
    g.locals.push_back(std::move(table));
}

void PhysicalHashAggregate::Finalize(GlobalSinkState& global) {
    ExecutionContext serial;
    FinalizeParallel(global, serial);
}

void PhysicalHashAggregate::FinalizeParallel(GlobalSinkState& global, ExecutionContext& context) {
    auto& g = static_cast<AggGlobalState&>(global);
    auto& locals = g.locals;
    g.tables.clear();
    if (locals.empty()) { // no thread ever produced a local state
        g.tables.push_back(std::make_unique<GroupTable>(GroupTypes(), Specs()));
        return;
    }
    idx_t total = 0;
    for (const auto& t : locals) {
        total += t->GroupCount();
    }
    const bool partitioned = locals.size() > 1 && !groups_.empty() && context.threads() > 1 &&
                             total >= MinGroupsToPartition() && locals[0]->CanCombineGroups();
    if (!partitioned) {
        // One table: adopt the first and merge the others into it (an ungrouped aggregate has one
        // group per table, so this is cheap).
        for (size_t i = 1; i < locals.size(); i++) {
            locals[0]->Combine(*locals[i]);
        }
        g.tables.push_back(std::move(locals[0]));
        locals.clear();
        return;
    }

    // Partition by the top bits of each group's hash (KeyIndex slots use the low bits, so the two
    // are independent). Several partitions per thread keep the tasks balanced when keys are skewed.
    size_t partitions = 16;
    while (partitions < context.threads() * 8 && partitions < 256) {
        partitions *= 2;
    }
    unsigned bits = 0;
    while ((size_t{1} << bits) < partitions) {
        bits++;
    }
    const unsigned shift = 64 - bits;

    // by_partition[t][p]: ids of table t's groups that fall in partition p.
    std::vector<std::vector<std::vector<uint32_t>>> by_partition(
        locals.size(), std::vector<std::vector<uint32_t>>(partitions));
    context.ParallelFor(locals.size(), [&](size_t t) {
        const GroupTable& table = *locals[t];
        for (idx_t id = 0; id < table.GroupCount(); id++) {
            by_partition[t][table.GroupHash(id) >> shift].push_back(static_cast<uint32_t>(id));
        }
    });

    std::vector<std::unique_ptr<GroupTable>> merged(partitions);
    context.ParallelFor(partitions, [&](size_t p) {
        auto table = std::make_unique<GroupTable>(GroupTypes(), Specs());
        for (size_t t = 0; t < locals.size(); t++) {
            const std::vector<uint32_t>& ids = by_partition[t][p];
            if (!ids.empty()) {
                table->CombineGroups(*locals[t], ids.data(), ids.size());
            }
        }
        merged[p] = std::move(table);
    });
    for (auto& table : merged) {
        if (table->GroupCount() > 0) {
            g.tables.push_back(std::move(table));
        }
    }
    locals.clear();
}

std::unique_ptr<GlobalSourceState>
PhysicalHashAggregate::GetGlobalSourceState(GlobalSinkState* sink_state) {
    auto s = std::make_unique<AggSourceState>();
    for (const auto& table : static_cast<AggGlobalState*>(sink_state)->tables) {
        for (idx_t first = 0; first < table->GroupCount(); first += kVectorSize) {
            s->ranges.push_back(
                {table.get(), first, std::min<idx_t>(kVectorSize, table->GroupCount() - first)});
        }
    }
    return s;
}

std::unique_ptr<LocalSourceState> PhysicalHashAggregate::GetLocalSourceState(GlobalSourceState&) {
    return std::make_unique<AggLocalSourceState>();
}

idx_t PhysicalHashAggregate::MaxSourceThreads(GlobalSourceState& global) const {
    return static_cast<AggSourceState&>(global).ranges.size();
}

bool PhysicalHashAggregate::GetData(GlobalSourceState& global, LocalSourceState& local,
                                    DataChunk& out) {
    auto& s = static_cast<AggSourceState&>(global);
    const size_t i = s.next.fetch_add(1, std::memory_order_relaxed);
    if (i >= s.ranges.size()) {
        return false;
    }
    const AggSourceState::Range& r = s.ranges[i];
    r.table->Scan(r.first, r.count, out);
    local.batch_index = i;
    return true;
}

} // namespace cdb
