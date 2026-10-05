#pragma once

#include "storage/column_builder.h"
#include "storage/column_definition.h"
#include "vector/data_chunk.h"

#include <memory>
#include <vector>

namespace cdb {

// An immutable horizontal partition of a table: one ColumnSegment per column, all with the same
// row count. The unit of zone-map pruning and (Phase 6) parallel scanning.
class RowGroup {
  public:
    explicit RowGroup(std::vector<std::shared_ptr<ColumnSegment>> columns);

    idx_t count() const noexcept { return count_; }
    idx_t ColumnCount() const noexcept { return columns_.size(); }
    const ColumnSegment& column(idx_t i) const { return *columns_.at(i); }
    size_t MemoryUsage() const noexcept;

  private:
    std::vector<std::shared_ptr<ColumnSegment>> columns_;
    idx_t count_;
};

// The mutable tail of a table that is still being filled.
class RowGroupBuilder {
  public:
    RowGroupBuilder(const std::vector<ColumnDefinition>& schema, idx_t max_rows);

    idx_t count() const noexcept { return count_; }
    idx_t max_rows() const noexcept { return max_rows_; }
    bool full() const noexcept { return count_ == max_rows_; }

    // Appends rows [offset, offset + n) of `chunk`. Requires n <= max_rows() - count().
    void Append(const DataChunk& chunk, idx_t offset, idx_t n);

    // Immutable copy of the rows so far; the builder keeps accepting rows.
    std::shared_ptr<const RowGroup> Snapshot() const;

    // Consumes the rows into an immutable RowGroup; the builder is empty afterwards.
    std::shared_ptr<const RowGroup> Seal();

  private:
    std::vector<ColumnBuilder> columns_;
    idx_t max_rows_;
    idx_t count_ = 0;
};

} // namespace cdb
