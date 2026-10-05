#include "vector/data_chunk.h"

#include <algorithm>
#include <sstream>

namespace cdb {

void DataChunk::Initialize(const std::vector<LogicalType>& types, idx_t capacity) {
    types_ = types;
    capacity_ = capacity;
    count_ = 0;
    columns_.clear();
    columns_.reserve(types.size());
    for (LogicalType t : types) {
        columns_.emplace_back(t, capacity);
    }
}

void DataChunk::SetCardinality(idx_t count) {
    CDB_CHECK(count <= capacity_);
    count_ = count;
}

void DataChunk::Reset() {
    for (Vector& v : columns_) {
        v.Reset();
    }
    count_ = 0;
}

void DataChunk::Flatten() {
    for (Vector& v : columns_) {
        v.Flatten(count_);
    }
}

void DataChunk::Slice(const SelectionVector& sel, idx_t count) {
    CDB_CHECK(count <= capacity_);
    for (Vector& v : columns_) {
        v.Slice(sel, count);
    }
    count_ = count;
}

void DataChunk::Append(const DataChunk& other) {
    CDB_CHECK(types_ == other.types_);
    CDB_CHECK(count_ + other.count_ <= capacity_);
    for (idx_t c = 0; c < columns_.size(); c++) {
        // Appending into a non-flat vector would require materialising it first.
        columns_[c].Flatten(count_);
        VectorOps::Copy(other.columns_[c], columns_[c], nullptr, other.count_, count_);
    }
    count_ += other.count_;
}

Value DataChunk::GetValue(idx_t column, idx_t row) const {
    CDB_CHECK(row < count_);
    return columns_.at(column).GetValue(row);
}

void DataChunk::SetValue(idx_t column, idx_t row, const Value& value) {
    CDB_CHECK(row < capacity_);
    columns_.at(column).SetValue(row, value);
}

std::string DataChunk::ToString() const {
    std::ostringstream out;
    out << "DataChunk(" << columns_.size() << " columns, " << count_ << " rows)\n";
    for (idx_t r = 0; r < count_; r++) {
        for (idx_t c = 0; c < columns_.size(); c++) {
            out << (c == 0 ? "  " : " | ") << columns_[c].GetValue(r).ToString();
        }
        out << "\n";
    }
    return out.str();
}

void DataChunk::Verify() const {
    CDB_CHECK(count_ <= capacity_);
    CDB_CHECK(types_.size() == columns_.size());
    for (idx_t c = 0; c < columns_.size(); c++) {
        CDB_CHECK(columns_[c].type() == types_[c]);
        columns_[c].Verify(count_);
    }
}

} // namespace cdb
