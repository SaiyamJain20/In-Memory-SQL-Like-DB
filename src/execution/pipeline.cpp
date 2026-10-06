#include "execution/pipeline.h"

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

void Executor::RunPipeline(const Pipeline& p) {
    // The sink's global state outlives the pipeline: a later pipeline reads it as its source (or,
    // for a join, probes it).
    auto& sink_slot = sinks_[p.sink];
    if (!sink_slot) {
        sink_slot = p.sink->GetGlobalSinkState();
    }
    GlobalSinkState& global_sink = *sink_slot;
    const std::unique_ptr<LocalSinkState> local_sink = p.sink->GetLocalSinkState(global_sink);

    const auto find_sink = [&](const PhysicalOperator* op) -> GlobalSinkState* {
        const auto it = sinks_.find(op);
        return it == sinks_.end() ? nullptr : it->second.get();
    };
    const std::unique_ptr<GlobalSourceState> global_source =
        p.source->GetGlobalSourceState(find_sink(p.source));
    const std::unique_ptr<LocalSourceState> local_source =
        p.source->GetLocalSourceState(*global_source);

    std::vector<std::unique_ptr<OperatorState>> states;
    std::vector<DataChunk> buffers(p.operators.size());
    for (size_t i = 0; i < p.operators.size(); i++) {
        states.push_back(p.operators[i]->GetOperatorState(find_sink(p.operators[i])));
        buffers[i].Initialize(p.operators[i]->types());
    }

    // Pushes `input` through operators[level..] into the sink. Returns false once nothing more is
    // wanted from the source (a satisfied LIMIT, or a sink that is full).
    const auto push = [&](auto&& self, const DataChunk& input, size_t level) -> bool {
        if (level == p.operators.size()) {
            return p.sink->Sink(global_sink, *local_sink, input) == SinkResult::NeedMoreInput;
        }
        DataChunk& output = buffers[level];
        for (;;) {
            output.Reset();
            const OperatorResult r = p.operators[level]->Execute(*states[level], input, output);
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

    DataChunk chunk;
    chunk.Initialize(p.source->types());
    for (;;) {
        chunk.Reset();
        if (!p.source->GetData(*global_source, *local_source, chunk)) {
            break;
        }
        if (chunk.size() == 0) {
            continue;
        }
        if (!push(push, chunk, 0)) {
            break;
        }
    }
    p.sink->Combine(global_sink, *local_sink);
    p.sink->Finalize(global_sink);
}

} // namespace cdb
