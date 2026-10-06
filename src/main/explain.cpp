#include "main/explain.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace cdb {

namespace {

std::string Rows(double rows) {
    return std::to_string(static_cast<long long>(std::llround(std::max(0.0, rows))));
}

std::string Ms(double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), ms < 10 ? "%.2f" : (ms < 100 ? "%.1f" : "%.0f"), ms);
    return std::string(buffer) + " ms";
}

double NanosToMs(uint64_t nanos) {
    return static_cast<double>(nanos) / 1e6;
}

void PrintPlan(const LogicalOperator& op, int depth, CardinalityEstimator& estimator,
               std::ostringstream& out) {
    out << std::string(static_cast<size_t>(depth) * 2, ' ') << op.Describe() << "  (~"
        << Rows(estimator.Rows(op)) << " rows)\n";
    for (const auto& child : op.children) {
        PrintPlan(*child, depth + 1, estimator, out);
    }
}

struct AnalyzePrinter {
    CardinalityEstimator& estimator;
    const PhysicalPlan& plan;
    const ExecutionProfile& profile;
    std::ostringstream out;

    void Print(const LogicalOperator& op, int depth, bool merged_into_parent) {
        out << std::string(static_cast<size_t>(depth) * 2, ' ') << op.Describe() << "  (";
        const double estimate = estimator.Rows(op);
        const PhysicalOperator* physical = plan.PhysicalFor(op);
        const OperatorProfile* p = physical != nullptr ? profile.Find(physical) : nullptr;
        if (merged_into_parent) {
            out << "sorted as a top-N together with the LIMIT above";
        } else if (p == nullptr) {
            out << "est ~" << Rows(estimate) << ", not run";
        } else {
            out << "est ~" << Rows(estimate) << ", actual " << p->rows_out.load() << " rows, "
                << Ms(NanosToMs(p->nanos.load()));
            const uint64_t consumed = p->rows_in.load();
            if (op.kind == LogicalKind::Join) {
                out << "; build " << consumed << " rows, " << Ms(NanosToMs(p->sink_nanos.load()));
            } else if (consumed > 0) {
                out << "; in " << consumed << " rows";
            }
        }
        out << ")\n";
        for (const auto& child : op.children) {
            const bool top_n = op.kind == LogicalKind::Limit && child->kind == LogicalKind::Order &&
                               physical != nullptr && plan.PhysicalFor(*child) == physical;
            Print(*child, depth + 1, top_n);
        }
    }
};

} // namespace

std::string ExplainPlan(const LogicalOperator& root, CardinalityEstimator& estimator) {
    std::ostringstream out;
    PrintPlan(root, 0, estimator, out);
    return out.str();
}

std::string ExplainAnalyze(const LogicalOperator& root, CardinalityEstimator& estimator,
                           const PhysicalPlan& plan, const ExecutionProfile& profile,
                           const AnalyzeSummary& summary) {
    AnalyzePrinter printer{estimator, plan, profile, {}};
    printer.Print(root, 0, false);
    printer.out << "Planning: " << Ms(summary.planning_ms) << "\n"
                << "Execution: " << Ms(summary.execution_ms) << " on " << summary.threads
                << (summary.threads == 1 ? " thread, " : " threads, ") << summary.rows_returned
                << (summary.rows_returned == 1 ? " row returned\n" : " rows returned\n")
                << "(operator times are summed over threads)\n";
    return printer.out.str();
}

} // namespace cdb
