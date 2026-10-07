#pragma once

#include "storage/column_segment.h"

#include <memory>

namespace cdb {

// Accumulates one column's rows (appended vector by vector) into growing flat buffers, then turns
// them into an immutable ColumnSegment. Not thread-safe; Table serialises access.
class ColumnBuilder {
  public:
    // `max_rows` must be a multiple of kVectorSize: the capacity of the row group being built.
    ColumnBuilder(LogicalType type, idx_t max_rows);

    LogicalType type() const noexcept { return type_; }
    idx_t count() const noexcept { return count_; }

    // Appends rows [src_offset, src_offset + n) of `src` (any vector format). Requires
    // count() + n <= max_rows.
    void Append(const Vector& src, idx_t src_offset, idx_t n);

    // An immutable copy of the rows appended so far. Out-of-line strings are shared with this
    // builder's (append-only) heap rather than copied. The builder is unchanged.
    std::shared_ptr<ColumnSegment> Snapshot() const;

    // Consumes the accumulated rows into a segment (seals the heap). The builder is empty
    // afterwards.
    std::shared_ptr<ColumnSegment> Seal();

  private:
    void Reserve(idx_t rows);
    std::shared_ptr<ColumnSegment> SnapshotSegment(bool with_distinct_sketch) const;

    LogicalType type_;
    idx_t max_rows_;
    idx_t capacity_ = 0; // rows allocated; always a multiple of kVectorSize
    idx_t count_ = 0;
    std::shared_ptr<Buffer> data_;
    ValidityMask validity_;
    std::shared_ptr<StringHeap> heap_;
};

} // namespace cdb
