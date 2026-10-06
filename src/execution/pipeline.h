#pragma once

#include "execution/physical_operator.h"

#include <atomic>
#include <unordered_map>

namespace cdb {

struct LogicalOperator;

// A chain  source -> operators -> sink  that runs without materialising anything in between,
// plus the pipelines that must finish first (the build side of a join, the input of a sort).
struct Pipeline {
    PhysicalOperator* source = nullptr;
    std::vector<PhysicalOperator*> operators;
    PhysicalOperator* sink = nullptr;
    std::vector<size_t> dependencies; // indexes into PhysicalPlan::pipelines
};

// A physical plan: the operators (owned here) and the pipelines they form, ordered so that every
// pipeline comes after its dependencies. `root` is the sink of the last pipeline (the result
// collector or an INSERT).
class PhysicalPlan {
  public:
    template <class T, class... Args> T& Make(Args&&... args) {
        auto op = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *op;
        operators_.push_back(std::move(op));
        return ref;
    }
    const std::vector<std::unique_ptr<PhysicalOperator>>& operators() const { return operators_; }

    std::vector<Pipeline> pipelines;
    PhysicalOperator* root = nullptr;

    // Which logical operator each physical one was planned from (a top-N stands for a LIMIT and the
    // ORDER BY under it): for EXPLAIN ANALYZE to show what ran next to what was planned. Not every
    // physical operator has one (the projection that restores a swapped join's column order).
    std::vector<std::pair<const LogicalOperator*, const PhysicalOperator*>> origins;
    const PhysicalOperator* PhysicalFor(const LogicalOperator& op) const {
        for (const auto& [logical, physical] : origins) {
            if (logical == &op) {
                return physical;
            }
        }
        return nullptr;
    }

    // Multi-line description: every pipeline with its source, operators and sink.
    std::string ToString() const;

  private:
    std::vector<std::unique_ptr<PhysicalOperator>> operators_;
};

// What an operator did in one run, for EXPLAIN ANALYZE. Times are summed over the threads that ran
// it (CPU time, not wall time) and cover the operator's own calls only, not what it pushed to the
// operators after it.
struct OperatorProfile {
    std::atomic<uint64_t> rows_in{
        0}; // rows consumed as a sink (a join's build side, a sort's input)
    std::atomic<uint64_t> rows_out{0};   // rows produced, as a source or a streaming operator
    std::atomic<uint64_t> nanos{0};      // all its calls
    std::atomic<uint64_t> sink_nanos{0}; // of which consuming input (Sink, Combine, Finalize)
};

class ExecutionProfile {
  public:
    // Registers every operator of `plan` up front, so that threads only ever update counters.
    explicit ExecutionProfile(const PhysicalPlan& plan) {
        for (const auto& op : plan.operators()) {
            ops_[op.get()];
        }
    }
    OperatorProfile* Find(const PhysicalOperator* op) {
        const auto it = ops_.find(op);
        return it == ops_.end() ? nullptr : &it->second;
    }
    const OperatorProfile* Find(const PhysicalOperator* op) const {
        const auto it = ops_.find(op);
        return it == ops_.end() ? nullptr : &it->second;
    }

  private:
    std::unordered_map<const PhysicalOperator*, OperatorProfile> ops_;
};

// Runs a PhysicalPlan: pipelines in order, each by pulling chunks from its source and pushing them
// through the streaming operators into the sink.
//
// With a scheduler, a pipeline whose source, operators and sink all allow it runs on several
// threads at once (morsel-driven parallelism): every thread executes the whole chain with its own
// local states, pulling the next morsel from the shared source whenever it is ready for more, and
// merges its local sink state into the global one when the source is exhausted. Pipelines still
// run one after another (each is parallel inside); a pipeline with an operator that cannot be
// parallelised, such as LIMIT, runs on the calling thread alone.
class Executor {
  public:
    // `scheduler` may be null (or have one thread): everything then runs on the calling thread.
    explicit Executor(PhysicalPlan& plan, TaskScheduler* scheduler = nullptr)
        : plan_(plan), scheduler_(scheduler) {}

    // Counts rows and times calls into `profile` (which must be built from this plan) during Run().
    void SetProfile(ExecutionProfile* profile) noexcept { profile_ = profile; }

    // Throws cdb::Error on a run-time failure; the plan's sink states are then discarded.
    void Run();

    // The global sink state of `op` after Run() (e.g. the result collector's rows).
    GlobalSinkState* SinkState(const PhysicalOperator& op) const;

  private:
    void RunPipeline(const Pipeline& pipeline);

    PhysicalPlan& plan_;
    TaskScheduler* scheduler_;
    ExecutionProfile* profile_ = nullptr;
    std::unordered_map<const PhysicalOperator*, std::unique_ptr<GlobalSinkState>> sinks_;
};

} // namespace cdb
