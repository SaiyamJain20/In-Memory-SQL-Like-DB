#include "main/query_result.h"

#include "common/assert.h"

#include <algorithm>

namespace cdb {

QueryResult QueryResult::Failure(ErrorCode code, std::string message) {
    QueryResult r;
    r.ok_ = false;
    r.error_code_ = code;
    r.error_message_ = std::move(message);
    return r;
}

QueryResult QueryResult::Success(std::vector<std::string> names, std::vector<LogicalType> types,
                                 std::vector<DataChunk> chunks) {
    QueryResult r;
    r.names_ = std::move(names);
    r.types_ = std::move(types);
    for (const DataChunk& c : chunks)
        r.row_count_ += c.size();
    r.chunks_ = std::move(chunks);
    return r;
}

QueryResult QueryResult::Empty() {
    return Success({}, {}, {});
}

Value QueryResult::GetValue(idx_t column, idx_t row) const {
    CDB_CHECK(ok_ && column < types_.size() && row < row_count_);
    for (const DataChunk& c : chunks_) {
        if (row < c.size())
            return c.GetValue(column, row);
        row -= c.size();
    }
    CDB_UNREACHABLE("QueryResult::GetValue");
}

std::vector<std::vector<Value>> QueryResult::Rows() const {
    std::vector<std::vector<Value>> rows;
    rows.reserve(row_count_);
    for (const DataChunk& c : chunks_) {
        for (idx_t r = 0; r < c.size(); r++) {
            std::vector<Value> row;
            for (idx_t col = 0; col < types_.size(); col++)
                row.push_back(c.GetValue(col, r));
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::string QueryResult::ToString() const {
    if (!ok_)
        return error_message_;
    if (types_.empty())
        return "";
    const auto rows = Rows();
    std::vector<size_t> width(types_.size());
    for (size_t c = 0; c < types_.size(); c++)
        width[c] = names_[c].size();
    std::vector<std::vector<std::string>> cells;
    for (const auto& row : rows) {
        std::vector<std::string> line;
        for (size_t c = 0; c < row.size(); c++) {
            std::string text = row[c].ToString();
            const size_t nl = text.find('\n');
            if (nl != std::string::npos)
                text = text.substr(0, nl) + "..."; // keep rows on one line
            width[c] = std::max(width[c], text.size());
            line.push_back(std::move(text));
        }
        cells.push_back(std::move(line));
    }
    auto pad = [](const std::string& s, size_t w) { return s + std::string(w - s.size(), ' '); };
    std::string out;
    for (size_t c = 0; c < types_.size(); c++) {
        out += (c ? " | " : " ") + pad(names_[c], width[c]);
    }
    out += "\n";
    for (size_t c = 0; c < types_.size(); c++) {
        out += std::string(c ? "-+-" : "-") + std::string(width[c], '-');
    }
    out += "\n";
    for (const auto& line : cells) {
        for (size_t c = 0; c < line.size(); c++) {
            out += (c ? " | " : " ") + pad(line[c], width[c]);
        }
        out += "\n";
    }
    out += "(" + std::to_string(rows.size()) + (rows.size() == 1 ? " row)" : " rows)");
    return out;
}

} // namespace cdb
