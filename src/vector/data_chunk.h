#pragma once

#include "common/types.h"
#include "types/logical_type.h"
#include "vector/vector.h"

#include <string>
#include <vector>

namespace cdb {

// A horizontal slice of a table: one Vector per column, all with `size()` valid rows. This is
// the unit of data that flows between operators.
class DataChunk {
  public:
    DataChunk() = default;

    // (Re)creates one Flat vector per type, each with room for `capacity` rows; size() becomes 0.
    void Initialize(const std::vector<LogicalType>& types, idx_t capacity = kVectorSize);

    idx_t size() const noexcept { return count_; }
    idx_t capacity() const noexcept { return capacity_; }
    idx_t ColumnCount() const noexcept { return columns_.size(); }
    const std::vector<LogicalType>& types() const noexcept { return types_; }

    void SetCardinality(idx_t count);

    Vector& column(idx_t i) {
        CDB_ASSERT(i < columns_.size());
        return columns_[i];
    }
    const Vector& column(idx_t i) const {
        CDB_ASSERT(i < columns_.size());
        return columns_[i];
    }

    // size() = 0 and every column back to an empty Flat vector (buffers reused when unshared).
    void Reset();

    // Flattens every column.
    void Flatten();

    // Keeps rows sel[0..count) of every column (zero-copy) and sets size() = count.
    void Slice(const SelectionVector& sel, idx_t count);

    // Appends `other`'s rows (copying). Requires identical types and enough capacity.
    void Append(const DataChunk& other);

    Value GetValue(idx_t column, idx_t row) const;
    void SetValue(idx_t column, idx_t row, const Value& value);

    // Pretty-printed table for debugging and test failure messages.
    std::string ToString() const;

    void Verify() const;

  private:
    std::vector<LogicalType> types_;
    std::vector<Vector> columns_;
    idx_t count_ = 0;
    idx_t capacity_ = 0;
};

} // namespace cdb
