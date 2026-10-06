#include "main/connection.h"

#include "execution/basic_operators.h"
#include "execution/physical_planner.h"
#include "io/csv_reader.h"
#include "main/explain.h"
#include "parser/parser.h"
#include "planner/binder.h"
#include "planner/optimizer.h"

#include <chrono>
#include <sstream>

namespace cdb {

namespace {

QueryResult CountResult(idx_t count) {
    DataChunk chunk;
    chunk.Initialize({LogicalType::BigInt()}, 1);
    chunk.SetValue(0, 0, Value::BigInt(static_cast<int64_t>(count)));
    chunk.SetCardinality(1);
    std::vector<DataChunk> chunks;
    chunks.push_back(std::move(chunk));
    return QueryResult::Success({"Count"}, {LogicalType::BigInt()}, std::move(chunks));
}

} // namespace

LogicalPtr Connection::Plan(std::string_view sql) {
    StatementPtr stmt = ParseStatement(sql);
    Binder binder(db_.catalog());
    return binder.Bind(*stmt);
}

std::vector<QueryResult> Connection::QueryAll(std::string_view sql) {
    std::vector<QueryResult> results;
    try {
        // The whole script is parsed first: a syntax error anywhere means nothing runs.
        std::vector<StatementPtr> statements = ParseStatements(sql);
        if (statements.empty()) {
            results.push_back(QueryResult::Failure(ErrorCode::Syntax, "Syntax Error: empty query"));
            return results;
        }
        for (const StatementPtr& stmt : statements) {
            // Each statement is bound just before it runs, so it sees the effects of earlier ones.
            Binder binder(db_.catalog());
            LogicalPtr plan = binder.Bind(*stmt);
            results.push_back(
                Execute(std::move(plan))); // failures throw: the catch below ends the script
        }
    } catch (const Error& e) {
        results.push_back(QueryResult::Failure(e.code(), FormatErrorWithContext(sql, e)));
    } catch (const std::exception& e) {
        results.push_back(
            QueryResult::Failure(ErrorCode::Internal, std::string("Internal Error: ") + e.what()));
    }
    return results;
}

QueryResult Connection::Query(std::string_view sql) {
    std::vector<QueryResult> all = QueryAll(sql);
    return std::move(all.back());
}

QueryResult Connection::Execute(LogicalPtr plan) {
    switch (plan->kind) {
    case LogicalKind::CreateTable: {
        const auto& op = static_cast<const LogicalCreateTable&>(*plan);
        db_.CreateTable(op.table_name, op.schema, op.if_not_exists);
        return QueryResult::Empty();
    }
    case LogicalKind::DropTable: {
        const auto& op = static_cast<const LogicalDropTable&>(*plan);
        db_.DropTable(op.table_name, op.if_exists);
        return QueryResult::Empty();
    }
    case LogicalKind::Insert:
        return ExecuteInsert(std::move(plan));
    case LogicalKind::Copy: {
        const auto& op = static_cast<const LogicalCopy&>(*plan);
        CsvOptions options;
        options.delimiter = op.delimiter.empty() ? ',' : op.delimiter[0];
        options.header = op.header;
        options.commit = [this, table = op.table](std::unique_ptr<Table> staging) {
            db_.CommitAppend(table, std::move(staging));
        };
        const std::shared_ptr<TaskScheduler> scheduler = db_.scheduler();
        return CountResult(LoadCsvFile(*op.table, op.path, options, scheduler.get()));
    }
    case LogicalKind::Checkpoint:
        db_.Checkpoint();
        return QueryResult::Empty();
    case LogicalKind::Explain: {
        const auto& op = static_cast<const LogicalExplain&>(*plan);
        // (Optimize is a no-op for DDL statements.)
        LogicalPtr inner =
            optimize_ ? Optimize(std::move(plan->children[0])) : std::move(plan->children[0]);
        CardinalityEstimator estimator;
        std::string text =
            op.analyze ? ExplainAnalyzeQuery(*inner, estimator) : ExplainPlan(*inner, estimator);
        std::vector<std::string> lines;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);)
            lines.push_back(line);
        DataChunk chunk;
        chunk.Initialize({LogicalType::Varchar()}, std::max<idx_t>(lines.size(), 1));
        for (idx_t i = 0; i < lines.size(); i++)
            chunk.SetValue(0, i, Value::Varchar(lines[i]));
        chunk.SetCardinality(lines.size());
        std::vector<DataChunk> chunks;
        chunks.push_back(std::move(chunk));
        return QueryResult::Success({"explain_value"}, {LogicalType::Varchar()}, std::move(chunks));
    }
    default:
        return ExecuteSelect(std::move(plan));
    }
}

std::string Connection::ExplainAnalyzeQuery(const LogicalOperator& optimized,
                                            CardinalityEstimator& estimator) {
    switch (optimized.kind) {
    case LogicalKind::Get:
    case LogicalKind::Filter:
    case LogicalKind::Projection:
    case LogicalKind::Aggregate:
    case LogicalKind::Join:
    case LogicalKind::Order:
    case LogicalKind::Limit:
    case LogicalKind::Distinct:
    case LogicalKind::Values:
    case LogicalKind::ScalarGuard:
        break;
    default:
        throw Error(ErrorCode::NotImplemented, "EXPLAIN ANALYZE is only supported for SELECT");
    }
    using Clock = std::chrono::steady_clock;
    const auto ms_since = [](Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };
    const auto planning_start = Clock::now();
    const std::unique_ptr<PhysicalPlan> physical = PlanSelect(optimized);
    ExecutionProfile profile(*physical);
    const double planning_ms = ms_since(planning_start);
    const std::shared_ptr<TaskScheduler> scheduler = db_.scheduler();
    Executor executor(*physical, scheduler.get());
    executor.SetProfile(&profile);
    const auto execution_start = Clock::now();
    executor.Run();
    AnalyzeSummary summary;
    summary.planning_ms = planning_ms;
    summary.execution_ms = ms_since(execution_start);
    summary.threads = scheduler != nullptr ? scheduler->threads() : 1;
    for (const DataChunk& chunk :
         PhysicalResultCollector::TakeChunks(*executor.SinkState(*physical->root))) {
        summary.rows_returned += chunk.size();
    }
    return ExplainAnalyze(optimized, estimator, *physical, profile, summary);
}

QueryResult Connection::ExecuteSelect(LogicalPtr plan) {
    LogicalPtr optimized = optimize_ ? Optimize(std::move(plan)) : std::move(plan);
    const std::unique_ptr<PhysicalPlan> physical = PlanSelect(*optimized);
    const std::shared_ptr<TaskScheduler> scheduler = db_.scheduler();
    Executor executor(*physical, scheduler.get());
    executor.Run();
    std::vector<DataChunk> chunks =
        PhysicalResultCollector::TakeChunks(*executor.SinkState(*physical->root));
    return QueryResult::Success(optimized->names, optimized->types, std::move(chunks));
}

QueryResult Connection::ExecuteInsert(LogicalPtr plan) {
    LogicalPtr optimized = optimize_ ? Optimize(std::move(plan)) : std::move(plan);
    const auto& insert = static_cast<const LogicalInsert&>(*optimized);
    const std::unique_ptr<PhysicalPlan> physical =
        PlanInsert(insert, [this, table = insert.table](std::unique_ptr<Table> staging) {
            db_.CommitAppend(table, std::move(staging));
        });
    const std::shared_ptr<TaskScheduler> scheduler = db_.scheduler();
    Executor executor(*physical, scheduler.get());
    executor.Run();
    return CountResult(PhysicalInsert::InsertedRows(*executor.SinkState(*physical->root)));
}

} // namespace cdb
