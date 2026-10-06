#include "execution/pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>

namespace cdb {

std::string PhysicalPlan::ToString() const {
    std::ostringstream out;
    for (size_t i = 0; i < pipelines.size(); i++) {
        const Pipeline& p = pipelines[i];
        out << "Pipeline " << i;
        if (!p.dependencies.empty()) {
            out << " (after";
            for (const size_t d : p.dependencies) {
                out << " " << d;
            }
            out << ")";
        }
        out << ": " << p.source->Describe();
        for (const PhysicalOperator* op : p.operators) {
            out << " -> " << op->Describe();
        }
        out << " => " << p.sink->Describe() << "\n";
    }
    return out.str();
}

GlobalSinkState* Executor::SinkState(const PhysicalOperator& op) const {
    const auto it = sinks_.find(&op);
    return it == sinks_.end() ? nullptr : it->second.get();
}

void Executor::Run() {
    sinks_.clear();
    for (const Pipeline& p : plan_.pipelines) {
        RunPipeline(p);
    }
}

namespace {

// Adds the time of a scope to an operator's profile (nothing when there is none).
class CallTimer {
  public:
    CallTimer(OperatorProfile* profile, bool sink_role) : profile_(profile), sink_role_(sink_role) {
        if (profile_ != nullptr) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~CallTimer() {
        if (profile_ != nullptr) {
            const auto ns =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start_)
                                          .count());
            profile_->nanos.fetch_add(ns, std::memory_order_relaxed);
            if (sink_role_) {
                profile_->sink_nanos.fetch_add(ns, std::memory_order_relaxed);
            }
        }
    }
    CallTimer(const CallTimer&) = delete;
    CallTimer& operator=(const CallTimer&) = delete;

  private:
    OperatorProfile* profile_;
    bool sink_role_;
    std::chrono::steady_clock::time_point start_;
};

// A pipeline may run on several threads only if every role in it allows it.
bool PipelineIsParallel(const Pipeline& p) {
    if (!p.source->ParallelSource() || !p.sink->ParallelSink()) {
        return false;
    }
    return std::all_of(p.operators.begin(), p.operators.end(),
                       [](const PhysicalOperator* op) { return op->ParallelOperator(); });
}

} // namespace

void Executor::RunPipeline(const Pipeline& p) {
    // The sink's global state outlives the pipeline: a later pipeline reads it as its source (or,
    // for a join, probes it).
    auto& sink_slot = sinks_[p.sink];
    if (!sink_slot) {
        sink_slot = p.sink->GetGlobalSinkState();
    }
    GlobalSinkState& global_sink = *sink_slot;

    OperatorProfile* const source_profile = profile_ ? profile_->Find(p.source) : nullptr;
    OperatorProfile* const sink_profile = profile_ ? profile_->Find(p.sink) : nullptr;
    std::vector<OperatorProfile*> operator_profiles;
    for (const PhysicalOperator* op : p.operators) {
        operator_profiles.push_back(profile_ ? profile_->Find(op) : nullptr);
    }

    const auto find_sink = [&](const PhysicalOperator* op) -> GlobalSinkState* {
        const auto it = sinks_.find(op);
        return it == sinks_.end() ? nullptr : it->second.get();
    };
    const bool parallel =
        scheduler_ != nullptr && scheduler_->threads() > 1 && PipelineIsParallel(p);
    p.source->SetThreadHint(parallel ? scheduler_->threads() : 1);
    const std::unique_ptr<GlobalSourceState> global_source =
        p.source->GetGlobalSourceState(find_sink(p.source));

    size_t participants = 1;
    if (parallel) {
        participants = std::clamp<size_t>(p.source->MaxSourceThreads(*global_source), 1,
                                          scheduler_->threads());
    }

    // Set when a participant failed or the sink wants no more input: everyone stops pulling.
    std::atomic<bool> stop{false};

    // What one thread does: its own local states, then pull-push until the source is exhausted.
    const auto participant = [&](size_t) {
        const std::unique_ptr<LocalSinkState> local_sink = p.sink->GetLocalSinkState(global_sink);
        const std::unique_ptr<LocalSourceState> local_source =
            p.source->GetLocalSourceState(*global_source);

        std::vector<std::unique_ptr<OperatorState>> states;
        std::vector<DataChunk> buffers(p.operators.size());
        for (size_t i = 0; i < p.operators.size(); i++) {
            states.push_back(p.operators[i]->GetOperatorState(find_sink(p.operators[i])));
            buffers[i].Initialize(p.operators[i]->types());
        }

        // Pushes `input` through operators[level..] into the sink. Returns false once nothing more
        // is wanted from the source (a satisfied LIMIT, or a sink that is full).
        const auto push = [&](auto&& self, const DataChunk& input, size_t level) -> bool {
            if (level == p.operators.size()) {
                if (sink_profile != nullptr) {
                    sink_profile->rows_in.fetch_add(input.size(), std::memory_order_relaxed);
                }
                const CallTimer timer(sink_profile, /*sink_role=*/true);
                return p.sink->Sink(global_sink, *local_sink, input) == SinkResult::NeedMoreInput;
            }
            DataChunk& output = buffers[level];
            OperatorProfile* const profile = operator_profiles[level];
            for (;;) {
                output.Reset();
                OperatorResult r;
                {
                    const CallTimer timer(profile, /*sink_role=*/false);
                    r = p.operators[level]->Execute(*states[level], input, output);
                }
                if (profile != nullptr) {
                    profile->rows_out.fetch_add(output.size(), std::memory_order_relaxed);
                }
                if (output.size() > 0 && !self(self, output, level + 1)) {
                    return false;
                }
                if (r == OperatorResult::Finished) {
                    return false;
                }
                if (r == OperatorResult::NeedMoreInput) {
                    return true;
                }
            }
        };

        try {
            DataChunk chunk;
            chunk.Initialize(p.source->types());
            while (!stop.load(std::memory_order_relaxed)) {
                chunk.Reset();
                bool more;
                {
                    const CallTimer timer(source_profile, /*sink_role=*/false);
                    more = p.source->GetData(*global_source, *local_source, chunk);
                }
                if (!more) {
                    break;
                }
                if (chunk.size() == 0) {
                    continue;
                }
                if (source_profile != nullptr) {
                    source_profile->rows_out.fetch_add(chunk.size(), std::memory_order_relaxed);
                }
                local_sink->batch_index = local_source->batch_index;
                if (!push(push, chunk, 0)) {
                    if (participants > 1) {
                        stop.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
            }
        } catch (...) {
            stop.store(true, std::memory_order_relaxed); // the others stop at their next chunk
            throw;
        }
        const CallTimer timer(sink_profile, /*sink_role=*/true);
        p.sink->Combine(global_sink, *local_sink);
    };

    if (participants == 1) {
        participant(0);
    } else {
        scheduler_->RunParallel(participants, participant);
    }
    ExecutionContext context{scheduler_};
    const CallTimer finalize_timer(sink_profile, /*sink_role=*/true);
    p.sink->FinalizeParallel(global_sink, context);
}

} // namespace cdb
