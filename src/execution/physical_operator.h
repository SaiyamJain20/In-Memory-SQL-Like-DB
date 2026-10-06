#pragma once

#include "common/error.h"
#include "execution/task_scheduler.h"
#include "vector/data_chunk.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cdb {

// State objects. "Global" state is shared by every thread working on a pipeline; "local" state is
// private to one thread. A parallel pipeline runs the same source -> operators -> sink chain on
// several threads at once, each with its own local source state, operator states and local sink
// state; threads meet only in the global states (a morsel cursor, a hash table being built) and in
// Combine().
struct GlobalSourceState {
    virtual ~GlobalSourceState() = default;
};
struct LocalSourceState {
    virtual ~LocalSourceState() = default;
    // Set by GetData(): which batch (a morsel of the scan, a range of an aggregate's groups, ...)
    // the chunk just returned belongs to. Batches are numbered in the order a single thread would
    // have produced them; all chunks of one batch come from one thread, in order. A sink that wants
    // results in a deterministic order sorts by it.
    idx_t batch_index = 0;
};
struct OperatorState {
    virtual ~OperatorState() = default;
};
struct GlobalSinkState {
    virtual ~GlobalSinkState() = default;
};
struct LocalSinkState {
    virtual ~LocalSinkState() = default;
    // The batch of the chunk about to be passed to Sink() (copied from the local source state by
    // the executor before each call).
    idx_t batch_index = 0;
};

// What an operator may use while finalizing: the scheduler, if the pipeline is allowed to use more
// than one thread. Without one everything runs inline on the calling thread.
struct ExecutionContext {
    TaskScheduler* scheduler = nullptr;

    size_t threads() const noexcept { return scheduler != nullptr ? scheduler->threads() : 1; }
    // Runs fn(i) for every i in [0, n), on up to threads() threads.
    void ParallelFor(size_t n, const std::function<void(size_t)>& fn) const {
        if (scheduler != nullptr && n > 1) {
            scheduler->ParallelFor(n, fn);
        } else {
            for (size_t i = 0; i < n; i++) {
                fn(i);
            }
        }
    }
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
    // Fills `out` (Reset, of types()) and returns true, or returns false when exhausted. Sets
    // local.batch_index.
    virtual bool GetData(GlobalSourceState& global, LocalSourceState& local, DataChunk& out);
    // True if GetData() may be called by several threads at once, each with its own local state
    // (the global state hands out disjoint work, e.g. with an atomic cursor).
    virtual bool ParallelSource() const { return false; }
    // How many threads can usefully share this source (e.g. its number of morsels).
    virtual idx_t MaxSourceThreads(GlobalSourceState&) const { return 1; }
    // Told by the executor, before GetGlobalSourceState(), how many threads will read the source,
    // so that it can cut its work into enough pieces (a table scan sizes its morsels by it).
    virtual void SetThreadHint(size_t) {}

    // ---- streaming role -----------------------------------------------------------------
    // `sink_state` is this operator's global sink state if it is also a sink in another pipeline
    // (a hash join's build side), else null.
    virtual std::unique_ptr<OperatorState> GetOperatorState(GlobalSinkState* sink_state);
    virtual OperatorResult Execute(OperatorState& state, const DataChunk& input, DataChunk& output);
    // True if one chunk's output depends only on that chunk and shared read-only state, so several
    // threads can run the operator on different chunks, each with its own OperatorState. False for
    // an operator that counts rows across chunks (LIMIT): its pipeline then runs on one thread.
    virtual bool ParallelOperator() const { return false; }

    // ---- sink role ----------------------------------------------------------------------
    virtual std::unique_ptr<GlobalSinkState> GetGlobalSinkState();
    virtual std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState& global);
    virtual SinkResult Sink(GlobalSinkState& global, LocalSinkState& local, const DataChunk& input);
    // True if Sink()/Combine() may be called by several threads at once, each with its own local
    // state (shared state is only touched under a lock or atomically).
    virtual bool ParallelSink() const { return false; }
    // Merges one thread's local state into the global state (called once per local state, possibly
    // from several threads at once).
    virtual void Combine(GlobalSinkState& global, LocalSinkState& local);
    // Called once after every Combine(), on one thread; builds whatever the next pipeline reads.
    virtual void Finalize(GlobalSinkState& global);
    // What the executor calls instead of Finalize(): for operators that can use several threads to
    // finalize (merging per-thread hash tables, building a join table). The default just calls
    // Finalize(global); an operator that overrides this implements Finalize(global) as the same
    // work without a scheduler.
    virtual void FinalizeParallel(GlobalSinkState& global, ExecutionContext& context);

  private:
    std::vector<LogicalType> types_;
};

} // namespace cdb
