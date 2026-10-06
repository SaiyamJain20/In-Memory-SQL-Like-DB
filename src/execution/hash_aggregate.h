#pragma once

#include "execution/group_table.h"
#include "execution/physical_operator.h"
#include "planner/bound_expression.h"

namespace cdb {

// GROUP BY (and, with no aggregates, DISTINCT): a sink that hashes rows into a GroupTable and then
// a source that emits one row per group - the group columns, then the aggregates. Each thread
// aggregates into its own table (local sink state) and hands it over in Combine. Finalize merges
// them: small results into one table, large ones by hash partition - the groups of every thread's
// table with the same hash bits are merged by one task, so partitions are merged in parallel with
// no locking and the result is one table per partition. The source then emits ranges of groups,
// also in parallel. With no group expressions there is exactly one output row, even for empty
// input.
class PhysicalHashAggregate final : public PhysicalOperator {
  public:
    // `aggregates` are BoundKind::Aggregate expressions (their single argument is evaluated per
    // input row). `types` = group types followed by the aggregates' result types.
    PhysicalHashAggregate(std::vector<LogicalType> types, std::vector<BoundExprPtr> groups,
                          std::vector<BoundExprPtr> aggregates);
    std::string Name() const override {
        return groups_.empty() ? "UNGROUPED_AGGREGATE" : "HASH_GROUP_BY";
    }
    std::string Describe() const override;

    // The number of groups (summed over the threads' tables) from which Finalize merges by hash
    // partition instead of into one table: 32768, or CDB_PARTITION_MIN_GROUPS from the environment,
    // or what SetMinGroupsToPartition() last set (tests lower it to exercise the partitioned merge
    // on small data; 0 restores the default).
    static idx_t MinGroupsToPartition() noexcept;
    static void SetMinGroupsToPartition(idx_t groups) noexcept;

    std::unique_ptr<GlobalSinkState> GetGlobalSinkState() override;
    std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState&) override;
    SinkResult Sink(GlobalSinkState&, LocalSinkState&, const DataChunk& input) override;
    bool ParallelSink() const override { return true; }
    void Combine(GlobalSinkState&, LocalSinkState&) override;
    void Finalize(GlobalSinkState&) override;
    void FinalizeParallel(GlobalSinkState&, ExecutionContext&) override;

    std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState* sink_state) override;
    std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override;
    bool GetData(GlobalSourceState&, LocalSourceState&, DataChunk& out) override;
    bool ParallelSource() const override { return true; }
    idx_t MaxSourceThreads(GlobalSourceState&) const override;

  private:
    std::vector<LogicalType> GroupTypes() const;
    std::vector<AggregateSpec> Specs() const;

    std::vector<BoundExprPtr> groups_;
    std::vector<BoundExprPtr> aggregates_;
};

} // namespace cdb
