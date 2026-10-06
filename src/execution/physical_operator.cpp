#include "execution/physical_operator.h"

namespace cdb {

namespace {
[[noreturn]] void NotSupported(const PhysicalOperator& op, const char* role) {
    throw Error(ErrorCode::Internal, op.Name() + " cannot act as a " + role);
}
} // namespace

std::unique_ptr<GlobalSourceState> PhysicalOperator::GetGlobalSourceState(GlobalSinkState*) {
    NotSupported(*this, "source");
}
std::unique_ptr<LocalSourceState> PhysicalOperator::GetLocalSourceState(GlobalSourceState&) {
    NotSupported(*this, "source");
}
bool PhysicalOperator::GetData(GlobalSourceState&, LocalSourceState&, DataChunk&) {
    NotSupported(*this, "source");
}
std::unique_ptr<OperatorState> PhysicalOperator::GetOperatorState(GlobalSinkState*) {
    NotSupported(*this, "streaming operator");
}
OperatorResult PhysicalOperator::Execute(OperatorState&, const DataChunk&, DataChunk&) {
    NotSupported(*this, "streaming operator");
}
std::unique_ptr<GlobalSinkState> PhysicalOperator::GetGlobalSinkState() {
    NotSupported(*this, "sink");
}
std::unique_ptr<LocalSinkState> PhysicalOperator::GetLocalSinkState(GlobalSinkState&) {
    NotSupported(*this, "sink");
}
SinkResult PhysicalOperator::Sink(GlobalSinkState&, LocalSinkState&, const DataChunk&) {
    NotSupported(*this, "sink");
}
void PhysicalOperator::Combine(GlobalSinkState&, LocalSinkState&) {
    NotSupported(*this, "sink");
}
void PhysicalOperator::Finalize(GlobalSinkState&) {
    NotSupported(*this, "sink");
}
void PhysicalOperator::FinalizeParallel(GlobalSinkState& global, ExecutionContext&) {
    Finalize(global);
}

} // namespace cdb
