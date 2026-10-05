#include "main/connection.h"

#include "io/csv_reader.h"
#include "parser/parser.h"
#include "planner/binder.h"
#include "planner/scalar_eval.h"

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
            results.push_back(Execute(*plan));
            if (!results.back().ok())
                break;
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

QueryResult Connection::Execute(const LogicalOperator& plan) {
    switch (plan.kind) {
    case LogicalKind::CreateTable: {
        const auto& op = static_cast<const LogicalCreateTable&>(plan);
        db_.catalog().CreateTable(op.table_name, op.schema, op.if_not_exists);
        return QueryResult::Empty();
    }
    case LogicalKind::DropTable: {
        const auto& op = static_cast<const LogicalDropTable&>(plan);
        db_.catalog().DropTable(op.table_name, op.if_exists);
        return QueryResult::Empty();
    }
    case LogicalKind::Insert:
        return ExecuteInsert(static_cast<const LogicalInsert&>(plan));
    case LogicalKind::Copy: {
        const auto& op = static_cast<const LogicalCopy&>(plan);
        CsvOptions options;
        options.delimiter = op.delimiter.empty() ? ',' : op.delimiter[0];
        options.header = op.header;
        return CountResult(LoadCsvFile(*op.table, op.path, options));
    }
    case LogicalKind::Explain: {
        const auto& op = static_cast<const LogicalExplain&>(plan);
        if (op.analyze) {
            throw Error(ErrorCode::NotImplemented,
                        "EXPLAIN ANALYZE needs the query executor (Phase 4)");
        }
        std::string text = op.children[0]->ToString();
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
        return ExecuteConstantSelect(plan);
    }
}

QueryResult Connection::ExecuteInsert(const LogicalInsert& insert) {
    const LogicalOperator& source = *insert.children[0];
    if (source.kind != LogicalKind::Values) {
        throw Error(ErrorCode::NotImplemented,
                    "INSERT ... SELECT needs the query executor (Phase 4)");
    }
    const auto& values = static_cast<const LogicalValues&>(source);
    // Evaluate every row first, then publish atomically through a staging table.
    auto staging = std::make_unique<Table>("insert_staging", insert.table->schema(),
                                           insert.table->row_group_size());
    std::vector<LogicalType> types;
    for (const auto& c : insert.table->schema())
        types.push_back(c.type);
    DataChunk chunk;
    chunk.Initialize(types);
    idx_t in_chunk = 0;
    for (const auto& row : values.rows) {
        for (idx_t c = 0; c < row.size(); c++) {
            chunk.SetValue(c, in_chunk, EvaluateConstant(*row[c]));
        }
        if (++in_chunk == kVectorSize) {
            chunk.SetCardinality(in_chunk);
            staging->Append(chunk);
            chunk.Reset();
            in_chunk = 0;
        }
    }
    if (in_chunk > 0) {
        chunk.SetCardinality(in_chunk);
        staging->Append(chunk);
    }
    insert.table->Merge(std::move(staging));
    return CountResult(values.rows.size());
}

// Handles `SELECT <constant expressions> [LIMIT n]`: Projection over a one-row Values.
QueryResult Connection::ExecuteConstantSelect(const LogicalOperator& plan) {
    const LogicalOperator* node = &plan;
    std::optional<int64_t> limit;
    int64_t offset = 0;
    if (node->kind == LogicalKind::Limit) {
        const auto& l = static_cast<const LogicalLimit&>(*node);
        limit = l.limit;
        offset = l.offset;
        node = node->children[0].get();
    }
    if (node->kind != LogicalKind::Projection || node->children[0]->kind != LogicalKind::Values) {
        throw Error(ErrorCode::NotImplemented,
                    "queries over tables need the vectorized executor (Phase 4); use EXPLAIN to "
                    "see the plan");
    }
    const auto& proj = static_cast<const LogicalProjection&>(*node);
    const bool emit = offset == 0 && (!limit || *limit > 0);
    DataChunk chunk;
    chunk.Initialize(proj.types, 1);
    if (emit) {
        for (idx_t c = 0; c < proj.exprs.size(); c++) {
            chunk.SetValue(c, 0, EvaluateConstant(*proj.exprs[c]));
        }
        chunk.SetCardinality(1);
    }
    std::vector<DataChunk> chunks;
    chunks.push_back(std::move(chunk));
    return QueryResult::Success(proj.names, proj.types, std::move(chunks));
}

} // namespace cdb
