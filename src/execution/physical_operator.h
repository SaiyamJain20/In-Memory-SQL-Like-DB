#pragma once

#include "common/error.h"
#include "vector/data_chunk.h"

#include <memory>
#include <string>
#include <vector>

namespace cdb {

// State objects. "Global" state is shared by every thread working on a pipeline; "local" state is
// private to one. Phase 4 runs each pipeline on a single thread (one local state), but operators
// are written to the full protocol - per-thread accumulation in local state, merged into global
// state in Combine() - so the Phase 6 scheduler only has to run several local states at once.
struct GlobalSourceState {
    virtual ~GlobalSourceState() = default;
};
struct LocalSourceState {
    virtual ~LocalSourceState() = default;
};
struct OperatorState {
    virtual ~OperatorState() = default;
};
struct GlobalSinkState {
    virtual ~GlobalSinkState() = default;
};
struct LocalSinkState {
    virtual ~LocalSinkState() = default;
};

// What a streaming operator says after Execute(): it wants the next input chunk, it has more
// output for the same input (call again), or it has produced everything it ever will (a satisfied
// LIMIT), so the pipeline may stop early.
enum class OperatorResult : uint8_t { NeedMoreInput, HaveMoreOutput, Finished };
enum class SinkResult : uint8_t { NeedMoreInput, Finished };

// A node of the physical plan. An operator plays one or more of three roles in a pipeline
//
//     Source  ->  streaming operator*  ->  Sink
//
//  * source:    produces chunks (a table scan, VALUES, or the output of a finished pipeline
//               breaker such as an aggregate's hash table or a sorted buffer);
//  * streaming: maps an input chunk to output chunks (filter, projection, the probe side of a
//               join, LIMIT);
//  * sink:      consumes chunks and accumulates state (the build side of a join, an aggregate, a
//               sort, the result collector). Pipeline breakers are a sink in one pipeline and the
//               source of the next.
//
// Only the methods of the roles an operator plays are overridden; the rest abort.
class PhysicalOperator {
  public:
    explicit PhysicalOperator(std::vector<LogicalType> types) : types_(std::move(types)) {}
    virtual ~PhysicalOperator() = default;
    PhysicalOperator(const PhysicalOperator&) = delete;
    PhysicalOperator& operator=(const PhysicalOperator&) = delete;

    // The output column types.
    const std::vector<LogicalType>& types() const noexcept { return types_; }

    virtual std::string Name() const = 0;
    // One line for plan displays (name plus its parameters).
    virtual std::string Describe() const { return Name(); }

    // ---- source role --------------------------------------------------------------------
    // `sink_state` is this operator's own global sink state when it is a pipeline breaker that
    // already consumed its input; null for plain sources.
    virtual std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState* sink_state);
    virtual std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState& global);
    // Fills `out` (Reset, of types()) and returns true, or returns false when exhausted.
    virtual bool GetData(GlobalSourceState& global, LocalSourceState& local, DataChunk& out);

    // ---- streaming role -----------------------------------------------------------------
    // `sink_state` is this operator's global sink state if it is also a sink in another pipeline
    // (a hash join's build side), else null.
    virtual std::unique_ptr<OperatorState> GetOperatorState(GlobalSinkState* sink_state);
    virtual OperatorResult Execute(OperatorState& state, const DataChunk& input, DataChunk& output);

    // ---- sink role ----------------------------------------------------------------------
    virtual std::unique_ptr<GlobalSinkState> GetGlobalSinkState();
    virtual std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState& global);
    virtual SinkResult Sink(GlobalSinkState& global, LocalSinkState& local, const DataChunk& input);
    // Merges one thread's local state into the global state (called once per local state).
    virtual void Combine(GlobalSinkState& global, LocalSinkState& local);
    // Called once after every Combine(); builds whatever the next pipeline reads.
    virtual void Finalize(GlobalSinkState& global);

  private:
    std::vector<LogicalType> types_;
};

} // namespace cdb
