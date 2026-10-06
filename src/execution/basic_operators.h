#pragma once

#include "execution/physical_operator.h"
#include "planner/bound_expression.h"
#include "storage/table.h"

#include <functional>
#include <mutex>
#include <optional>

namespace cdb {

// Scans a table snapshot (taken when the plan was built, so every scan of one table in a query
// sees the same rows). Zone maps prune whole row groups using `filters`; rows of the groups that
// are read are not filtered here. The snapshot is cut into morsels that any number of threads claim
// from a shared cursor, so the scan is the source of a parallel pipeline.
class PhysicalTableScan final : public PhysicalOperator {
  public:
    PhysicalTableScan(std::string table_name, std::shared_ptr<const TableSnapshot> snapshot,
                      std::vector<idx_t> column_ids, std::vector<TableFilter> filters,
                      std::vector<LogicalType> types);
    std::string Name() const override { return "TABLE_SCAN"; }
    std::string Describe() const override;

    std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState*) override;
    std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override;
    bool GetData(GlobalSourceState&, LocalSourceState&, DataChunk& out) override;
    bool ParallelSource() const override { return true; }
    idx_t MaxSourceThreads(GlobalSourceState&) const override;
    void SetThreadHint(size_t threads) override { threads_ = threads; }

  private:
    std::string table_name_;
    size_t threads_ = 1;
    std::shared_ptr<const TableSnapshot> snapshot_;
    std::vector<idx_t> column_ids_;
    std::vector<TableFilter> filters_;
};

// Literal rows (VALUES, or the single empty row under `SELECT 1`).
class PhysicalValues final : public PhysicalOperator {
  public:
    PhysicalValues(std::vector<LogicalType> types, std::vector<std::vector<BoundExprPtr>> rows);
    std::string Name() const override { return "VALUES"; }
    std::string Describe() const override;

    std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState*) override;
    std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override;
    bool GetData(GlobalSourceState&, LocalSourceState&, DataChunk& out) override;

  private:
    std::vector<std::vector<BoundExprPtr>> rows_;
};

// Keeps the rows for which the predicate is TRUE. Output vectors are zero-copy dictionary views of
// the input (or the input itself when every row passes).
class PhysicalFilter final : public PhysicalOperator {
  public:
    PhysicalFilter(std::vector<LogicalType> types, BoundExprPtr predicate);
    std::string Name() const override { return "FILTER"; }
    std::string Describe() const override;

    std::unique_ptr<OperatorState> GetOperatorState(GlobalSinkState*) override;
    OperatorResult Execute(OperatorState&, const DataChunk& input, DataChunk& output) override;
    bool ParallelOperator() const override { return true; }

  private:
    BoundExprPtr predicate_;
};

class PhysicalProjection final : public PhysicalOperator {
  public:
    PhysicalProjection(std::vector<LogicalType> types, std::vector<BoundExprPtr> exprs);
    std::string Name() const override { return "PROJECTION"; }
    std::string Describe() const override;

    std::unique_ptr<OperatorState> GetOperatorState(GlobalSinkState*) override;
    OperatorResult Execute(OperatorState&, const DataChunk& input, DataChunk& output) override;
    bool ParallelOperator() const override { return true; }

  private:
    std::vector<BoundExprPtr> exprs_;
};

// LIMIT / OFFSET without ordering. Reports Finished as soon as the limit is met so the pipeline
// stops reading. It counts rows across chunks, so a pipeline containing it runs on one thread
// (which also keeps "the first N rows" well defined and the early exit cheap).
class PhysicalLimit final : public PhysicalOperator {
  public:
    PhysicalLimit(std::vector<LogicalType> types, std::optional<int64_t> limit, int64_t offset)
        : PhysicalOperator(std::move(types)), limit_(limit), offset_(offset) {}
    std::string Name() const override { return "LIMIT"; }
    std::string Describe() const override;

    std::unique_ptr<OperatorState> GetOperatorState(GlobalSinkState*) override;
    OperatorResult Execute(OperatorState&, const DataChunk& input, DataChunk& output) override;

  private:
    std::optional<int64_t> limit_;
    int64_t offset_;
};

// The root of a SELECT: materialises every chunk it is given as flat chunks of exactly their size.
// Chunks are returned in batch order (the order a single thread would have produced them), so a
// pipeline that is just scan / filter / projection yields rows in table order on any thread count.
class PhysicalResultCollector final : public PhysicalOperator {
  public:
    explicit PhysicalResultCollector(std::vector<LogicalType> types)
        : PhysicalOperator(std::move(types)) {}
    std::string Name() const override { return "RESULT"; }

    std::unique_ptr<GlobalSinkState> GetGlobalSinkState() override;
    std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState&) override;
    SinkResult Sink(GlobalSinkState&, LocalSinkState&, const DataChunk& input) override;
    bool ParallelSink() const override { return true; }
    void Combine(GlobalSinkState&, LocalSinkState&) override;
    void Finalize(GlobalSinkState&) override {}

    // Moves the collected chunks out of the (finished) global state, in batch order.
    static std::vector<DataChunk> TakeChunks(GlobalSinkState& global);
};

// The root of INSERT ... SELECT: collects rows in a private staging table and publishes them to
// the target with one atomic Table::Merge in Finalize, so a failure part-way leaves the target
// untouched and readers never see a partial insert. Rows are staged in batch order, so the target
// ends up with the rows in the order a single thread would have inserted them.
class PhysicalInsert final : public PhysicalOperator {
  public:
    // `commit` receives the staging table (all rows validated, in table order) once the input is
    // exhausted, and must make it part of the target - atomically; empty: Table::Merge. A
    // persistent database logs the rows there first.
    using Commit = std::function<void(std::unique_ptr<Table> staging)>;
    explicit PhysicalInsert(std::shared_ptr<Table> target, Commit commit = {})
        : PhysicalOperator({LogicalType::BigInt()}), target_(std::move(target)),
          commit_(std::move(commit)) {}
    std::string Name() const override { return "INSERT"; }
    std::string Describe() const override { return "INSERT " + target_->name(); }

    std::unique_ptr<GlobalSinkState> GetGlobalSinkState() override;
    std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState&) override;
    SinkResult Sink(GlobalSinkState&, LocalSinkState&, const DataChunk& input) override;
    bool ParallelSink() const override { return true; }
    void Combine(GlobalSinkState&, LocalSinkState&) override;
    void Finalize(GlobalSinkState&) override;

    // Rows inserted (valid after Finalize).
    static idx_t InsertedRows(GlobalSinkState& global);

  private:
    std::shared_ptr<Table> target_;
    Commit commit_;
};

} // namespace cdb
