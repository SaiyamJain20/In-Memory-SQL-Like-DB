#include "execution/scalar_guard.h"

#include "common/error.h"

namespace cdb {

namespace {

struct GuardSinkState final : GlobalSinkState {
    explicit GuardSinkState(const std::vector<LogicalType>& types) { row.Initialize(types, 1); }
    idx_t rows = 0;
    DataChunk row; // the one row, once there is one
};
struct GuardLocalSink final : LocalSinkState {};

struct GuardSource final : GlobalSourceState {
    explicit GuardSource(GuardSinkState* s) : sink(s) {}
    GuardSinkState* sink;
    bool done = false;
};
struct GuardLocalSource final : LocalSourceState {};

} // namespace

std::unique_ptr<GlobalSinkState> PhysicalScalarGuard::GetGlobalSinkState() {
    return std::make_unique<GuardSinkState>(types());
}
std::unique_ptr<LocalSinkState> PhysicalScalarGuard::GetLocalSinkState(GlobalSinkState&) {
    return std::make_unique<GuardLocalSink>();
}

SinkResult PhysicalScalarGuard::Sink(GlobalSinkState& global, LocalSinkState&,
                                     const DataChunk& input) {
    auto& g = static_cast<GuardSinkState&>(global);
    if (input.size() == 0) {
        return SinkResult::NeedMoreInput;
    }
    if (g.rows + input.size() > 1) {
        throw Error(ErrorCode::Execution,
                    "More than one row returned by a subquery used as an expression");
    }
    for (idx_t c = 0; c < input.ColumnCount(); c++) {
        g.row.SetValue(c, 0, input.GetValue(c, 0));
    }
    g.row.SetCardinality(1);
    g.rows = 1;
    return SinkResult::NeedMoreInput;
}

std::unique_ptr<GlobalSourceState>
PhysicalScalarGuard::GetGlobalSourceState(GlobalSinkState* sink) {
    return std::make_unique<GuardSource>(static_cast<GuardSinkState*>(sink));
}
std::unique_ptr<LocalSourceState> PhysicalScalarGuard::GetLocalSourceState(GlobalSourceState&) {
    return std::make_unique<GuardLocalSource>();
}

bool PhysicalScalarGuard::GetData(GlobalSourceState& global, LocalSourceState&, DataChunk& out) {
    auto& g = static_cast<GuardSource&>(global);
    if (g.done) {
        return false;
    }
    g.done = true;
    for (idx_t c = 0; c < out.ColumnCount(); c++) {
        out.SetValue(c, 0,
                     g.sink->rows == 1 ? g.sink->row.GetValue(c, 0) : Value::Null(types()[c]));
    }
    out.SetCardinality(1);
    return true;
}

} // namespace cdb
