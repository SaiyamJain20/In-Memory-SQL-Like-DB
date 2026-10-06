#pragma once

#include "common/error.h"
#include "vector/data_chunk.h"

#include <optional>
#include <string>
#include <vector>

namespace cdb {

// The outcome of Connection::Query(): either an error (never an exception across the API) or a
// table of results held as flat DataChunks.
class QueryResult {
  public:
    static QueryResult Failure(ErrorCode code, std::string message);
    static QueryResult Success(std::vector<std::string> names, std::vector<LogicalType> types,
                               std::vector<DataChunk> chunks);
    // A successful result with no columns (DDL).
    static QueryResult Empty();

    bool ok() const noexcept { return ok_; }
    // Failure details: the code, and the message including the SQL line and caret when known.
    ErrorCode error_code() const noexcept { return error_code_; }
    const std::string& error_message() const noexcept { return error_message_; }

    const std::vector<std::string>& names() const noexcept { return names_; }
    const std::vector<LogicalType>& types() const noexcept { return types_; }
    idx_t ColumnCount() const noexcept { return types_.size(); }
    idx_t RowCount() const noexcept { return row_count_; }
    Value GetValue(idx_t column, idx_t row) const;
    // Row-major copy of all values (convenient for tests and small results).
    std::vector<std::vector<Value>> Rows() const;

    // psql-style rendering (or the error message for a failed result). With `max_rows` set, only
    // that many rows are shown and the footer says how many were left out.
    std::string ToString(std::optional<idx_t> max_rows = std::nullopt) const;

  private:
    bool ok_ = true;
    ErrorCode error_code_ = ErrorCode::Internal;
    std::string error_message_;
    std::vector<std::string> names_;
    std::vector<LogicalType> types_;
    std::vector<DataChunk> chunks_;
    idx_t row_count_ = 0;
};

} // namespace cdb
