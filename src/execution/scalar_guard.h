#pragma once

#include "execution/physical_operator.h"

namespace cdb {

// The value of a scalar subquery that is not statically a single row: collects what its child
// produces and then yields exactly one row - the child's row; a row of NULLs if there was none; an
// error ("more than one row returned by a subquery used as an expression") if there were several.
// Both a sink (of the subquery's pipeline) and the source of the pipeline that joins the value in.
class PhysicalScalarGuard final : public PhysicalOperator {
  public:
    explicit PhysicalScalarGuard(std::vector<LogicalType> types)
        : PhysicalOperator(std::move(types)) {}
    std::string Name() const override { return "SCALAR_GUARD"; }

    std::unique_ptr<GlobalSinkState> GetGlobalSinkState() override;
    std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState&) override;
    SinkResult Sink(GlobalSinkState&, LocalSinkState&, const DataChunk& input) override;
    void Combine(GlobalSinkState&, LocalSinkState&) override {}
    void Finalize(GlobalSinkState&) override {}

    std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState* sink) override;
    std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override;
    bool GetData(GlobalSourceState&, LocalSourceState&, DataChunk& out) override;
};

} // namespace cdb
