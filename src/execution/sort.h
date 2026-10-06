#pragma once

#include "execution/chunk_store.h"
#include "execution/physical_operator.h"
#include "planner/logical_plan.h"

#include <optional>

namespace cdb {

// How one sort key orders rows. NULLs go first or last independently of the direction.
struct SortSpec {
    LogicalType type = LogicalType::Integer();
    bool descending = false;
    bool nulls_first = false;
};

// Rows (the payload columns an operator passes on) together with the evaluated sort-key columns,
// sortable by those keys. The sort is stable, so rows with equal keys keep arrival order and the
// output is deterministic.
class SortBuffer {
  public:
    SortBuffer(std::vector<LogicalType> payload_types, std::vector<SortSpec> keys);

    idx_t Count() const noexcept { return store_.Count(); }

    // Appends rows [0, payload.size()) of `payload` and the matching rows of `key_columns` (one
    // column per sort key).
    void Append(const DataChunk& payload, const DataChunk& key_columns);
    void AppendBuffer(const SortBuffer& other);
    // Moves every completely filled chunk of `other` in without copying a row (this buffer must
    // hold a multiple of kVectorSize rows); `other` keeps its partial last chunk, which
    // AppendBuffer() then copies. Merging many buffers copies at most one partial chunk each.
    void AdoptFullChunks(SortBuffer& other);

    // Orders the rows. After this, Scan() reads rows in sorted order. Append() may be called
    // again afterwards; the next Sort() re-sorts everything.
    void Sort();
    // The same order (a stable sort: equal keys keep their order in the buffer), computed on
    // several threads: ranges are sorted independently and then merged pairwise, each merge cut
    // into independent slices by binary search, so the last (largest) merges parallelise too.
    void SortParallel(const ExecutionContext& context);

    // The row count from which SortParallel() uses threads: 32768, or CDB_SORT_PARALLEL_MIN_ROWS
    // from the environment, or what SetMinRowsToSortInParallel() last set (tests lower it; 0
    // restores the default).
    static idx_t MinRowsToSortInParallel() noexcept;
    static void SetMinRowsToSortInParallel(idx_t rows) noexcept;

    // Sorts and drops all but the first `keep` rows.
    void SortAndKeep(idx_t keep);

    // Sorted rows [first, first + n) as payload columns of `out` (Initialize()d with the payload
    // types). Requires Sort().
    void Scan(idx_t first, idx_t n, DataChunk& out) const;

  private:
    int CompareRows(uint32_t a, uint32_t b) const;

    std::vector<LogicalType> payload_types_;
    std::vector<SortSpec> keys_;
    ChunkStore store_; // [payload columns..., key columns...]
    std::vector<uint32_t> order_;
};

// ORDER BY: a sink that buffers every row, sorts in Finalize, and a source that emits them in
// order. Threads fill their own buffers; Finalize moves them into one (whole chunks, no copying)
// and sorts it in parallel. Rows with equal keys keep their order in that combined buffer, which
// depends on which thread handed its buffer over first, so their relative order is not defined
// across runs when more than one thread feeds the sort.
class PhysicalOrder : public PhysicalOperator {
  public:
    PhysicalOrder(std::vector<LogicalType> types, std::vector<SortKey> keys);
    std::string Name() const override { return "ORDER_BY"; }
    std::string Describe() const override;

    std::unique_ptr<GlobalSinkState> GetGlobalSinkState() override;
    std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState&) override;
    SinkResult Sink(GlobalSinkState&, LocalSinkState&, const DataChunk& input) override;
    bool ParallelSink() const override { return true; }
    void Combine(GlobalSinkState&, LocalSinkState&) override;
    void Finalize(GlobalSinkState&) override;
    void FinalizeParallel(GlobalSinkState&, ExecutionContext&) override;

    std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState*) override;
    std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override;
    bool GetData(GlobalSourceState&, LocalSourceState&, DataChunk& out) override;
    bool ParallelSource() const override { return true; }
    idx_t MaxSourceThreads(GlobalSourceState&) const override;

  protected:
    // [first, last) of the sorted rows to emit.
    virtual void OutputRange(idx_t total, idx_t& first, idx_t& last) const {
        first = 0;
        last = total;
    }
    // Called after each Sink on the thread's local buffer (TopN prunes here).
    virtual void AfterSink(SortBuffer&) const {}
    // Called on a thread's buffer, on that thread, before it is handed over (TopN keeps only the
    // rows that can matter, so the merged buffer stays small).
    virtual void ReduceLocal(SortBuffer&) const {}
    // Called when sorting the combined buffer for the final output (TopN keeps only what it needs).
    virtual void SortFinal(SortBuffer& buffer, const ExecutionContext& context) const {
        buffer.SortParallel(context);
    }

    std::vector<SortKey> keys_;
    std::vector<SortSpec> specs_;
};

// ORDER BY ... LIMIT n [OFFSET m]: keeps only the best n + m rows while consuming input, instead of
// buffering and sorting everything.
class PhysicalTopN final : public PhysicalOrder {
  public:
    PhysicalTopN(std::vector<LogicalType> types, std::vector<SortKey> keys, int64_t limit,
                 int64_t offset);
    std::string Name() const override { return "TOP_N"; }
    std::string Describe() const override;

  protected:
    void OutputRange(idx_t total, idx_t& first, idx_t& last) const override;
    void AfterSink(SortBuffer& buffer) const override;
    void ReduceLocal(SortBuffer& buffer) const override;
    void SortFinal(SortBuffer& buffer, const ExecutionContext& context) const override;

  private:
    idx_t Keep() const;
    int64_t limit_;
    int64_t offset_;
};

} // namespace cdb
