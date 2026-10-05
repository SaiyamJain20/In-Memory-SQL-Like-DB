#include "storage/row_group.h"

namespace cdb {

RowGroup::RowGroup(std::vector<std::shared_ptr<ColumnSegment>> columns)
    : columns_(std::move(columns)), count_(0) {
    CDB_CHECK(!columns_.empty());
    count_ = columns_[0]->count();
    for (const auto& c : columns_) {
        CDB_CHECK(c->count() == count_);
    }
}

size_t RowGroup::MemoryUsage() const noexcept {
    size_t bytes = 0;
    for (const auto& c : columns_) {
        bytes += c->MemoryUsage();
    }
    return bytes;
}

RowGroupBuilder::RowGroupBuilder(const std::vector<ColumnDefinition>& schema, idx_t max_rows)
    : max_rows_(max_rows) {
    columns_.reserve(schema.size());
    for (const ColumnDefinition& c : schema) {
        columns_.emplace_back(c.type, max_rows);
    }
}

void RowGroupBuilder::Append(const DataChunk& chunk, idx_t offset, idx_t n) {
    CDB_CHECK(chunk.ColumnCount() == columns_.size());
    CDB_CHECK(count_ + n <= max_rows_ && offset + n <= chunk.size());
    for (idx_t c = 0; c < columns_.size(); c++) {
        columns_[c].Append(chunk.column(c), offset, n);
    }
    count_ += n;
}

std::shared_ptr<const RowGroup> RowGroupBuilder::Snapshot() const {
    std::vector<std::shared_ptr<ColumnSegment>> segments;
    segments.reserve(columns_.size());
    for (const ColumnBuilder& c : columns_) {
        segments.push_back(c.Snapshot());
    }
    return std::make_shared<RowGroup>(std::move(segments));
}

std::shared_ptr<const RowGroup> RowGroupBuilder::Seal() {
    std::vector<std::shared_ptr<ColumnSegment>> segments;
    segments.reserve(columns_.size());
    for (ColumnBuilder& c : columns_) {
        segments.push_back(c.Seal());
    }
    count_ = 0;
    return std::make_shared<RowGroup>(std::move(segments));
}

} // namespace cdb
