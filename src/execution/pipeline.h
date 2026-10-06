#pragma once

#include "execution/physical_operator.h"

#include <unordered_map>

namespace cdb {

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

    std::vector<Pipeline> pipelines;
    PhysicalOperator* root = nullptr;

    // Multi-line description: every pipeline with its source, operators and sink.
    std::string ToString() const;

  private:
    std::vector<std::unique_ptr<PhysicalOperator>> operators_;
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

    // Throws cdb::Error on a run-time failure; the plan's sink states are then discarded.
    void Run();

    // The global sink state of `op` after Run() (e.g. the result collector's rows).
    GlobalSinkState* SinkState(const PhysicalOperator& op) const;

  private:
    void RunPipeline(const Pipeline& pipeline);

    PhysicalPlan& plan_;
    TaskScheduler* scheduler_;
    std::unordered_map<const PhysicalOperator*, std::unique_ptr<GlobalSinkState>> sinks_;
};

} // namespace cdb
