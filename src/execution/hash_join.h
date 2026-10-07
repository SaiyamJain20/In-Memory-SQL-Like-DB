#pragma once

#include "execution/physical_operator.h"
#include "planner/bound_expression.h"

namespace cdb {

enum class PhysicalJoinType : uint8_t {
    Inner,
    Left, // every left row; unmatched ones get NULLs for the right columns
    Semi, // left rows that have at least one match; only the left columns
    Anti, // left rows that have no match; only the left columns
    // `x NOT IN (SELECT y ...)` with SQL's NULL rules, one key: a left row survives iff the build
    // side is empty, or its key is not NULL, nothing matches it and the build side has no NULL key
    // (a NULL among the y makes "x <> y" unknown for every x that matches none of the others).
    AntiNullAware,
};

// Equi-join (and, with no keys, nested-loop join) of a streaming left/probe side and a right/build
// side that is materialised first.
//
//  * Build (sink): the right input is appended to a row store; rows whose key has a NULL are
//    dropped (NULL never equals anything). Finalize hashes the keys into a bucket array with
//    chains in build order.
//  * Probe (streaming operator): each left row walks its bucket's chain; candidates must match on
//    hash and on every key, then on the optional residual predicate (evaluated over left ++ right
//    columns). Output is produced in batches of up to a vector, resuming mid-chain, so a probe row
//    with many matches never overflows a chunk.
//  * With no keys every build row is a candidate (cross join, or a join on a non-equality
//    condition expressed as the residual).
//
// Output columns: left ++ right for Inner/Left; left only for Semi/Anti. Row order follows the
// probe side, then build order within a probe row's matches.
class PhysicalHashJoin final : public PhysicalOperator {
  public:
    PhysicalHashJoin(std::vector<LogicalType> types, PhysicalJoinType join_type,
                     std::vector<LogicalType> left_types, std::vector<LogicalType> right_types,
                     std::vector<BoundExprPtr> left_keys, std::vector<BoundExprPtr> right_keys,
                     BoundExprPtr residual);
    std::string Name() const override;
    std::string Describe() const override;

    // The build side size from which Finalize builds the hash table with several threads (32768
    // rows, or CDB_JOIN_PARALLEL_MIN_ROWS from the environment, or what SetMinRowsToParallelize()
    // last set; tests lower it to exercise the parallel build on small data, 0 restores the
    // default).
    static idx_t MinRowsToParallelize() noexcept;
    static void SetMinRowsToParallelize(idx_t rows) noexcept;

    // build side (sink)
    std::unique_ptr<GlobalSinkState> GetGlobalSinkState() override;
    std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState&) override;
    SinkResult Sink(GlobalSinkState&, LocalSinkState&, const DataChunk& input) override;
    bool ParallelSink() const override { return true; }
    void Combine(GlobalSinkState&, LocalSinkState&) override;
    void Finalize(GlobalSinkState&) override;
    void FinalizeParallel(GlobalSinkState&, ExecutionContext&) override;

    // probe side (streaming)
    std::unique_ptr<OperatorState> GetOperatorState(GlobalSinkState* sink_state) override;
    OperatorResult Execute(OperatorState&, const DataChunk& input, DataChunk& output) override;
    bool ParallelOperator() const override { return true; } // probes read the finished build side

  private:
    PhysicalJoinType join_type_;
    std::vector<LogicalType> left_types_, right_types_;
    std::vector<BoundExprPtr> left_keys_, right_keys_;
    BoundExprPtr residual_;
};

} // namespace cdb
