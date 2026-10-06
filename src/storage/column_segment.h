#pragma once

#include "storage/column_stats.h"
#include "vector/vector.h"

#include <atomic>
#include <memory>
#include <vector>

namespace cdb {

// An immutable run of up to a row group's worth of one column, plus its zone-map statistics.
// Created by ColumnBuilder; safe to read from any number of threads.
//
// A raw segment stores values uncompressed, in the same layout as a flat Vector, so a scan is
// zero-copy: Scan() points the output vector at read-only Views into the segment's buffers. An
// encoded segment (storage/encoding.h) holds the same rows in a smaller form and decodes each
// vector into the output on scan; its zone-map statistics and validity are unchanged.
class EncodedColumn;

class ColumnSegment {
  public:
    // `data` must hold AlignUp(count, kVectorSize) elements and `validity` (if it has storage)
    // at least that many bits. The heap, if any, must be append-only-stable: either sealed, or the
    // still-growing heap of a ColumnBuilder (whose existing strings never move).
    ColumnSegment(LogicalType type, idx_t count, std::shared_ptr<Buffer> data,
                  ValidityMask validity, std::shared_ptr<StringHeap> heap, ColumnStats stats);

    // An encoded segment: `validity` must hold at least AlignUp(count, kVectorSize) bits if it has
    // storage. The values live in `encoded`.
    ColumnSegment(LogicalType type, idx_t count, ValidityMask validity, ColumnStats stats,
                  std::shared_ptr<const EncodedColumn> encoded);

    LogicalType type() const noexcept { return type_; }
    idx_t count() const noexcept { return count_; }
    const ColumnStats& stats() const noexcept { return stats_; }

    bool encoded() const noexcept { return encoded_ != nullptr; }
    const EncodedColumn* encoding() const noexcept { return encoded_.get(); }

    // The raw layout (only for a raw segment): used by the encoder.
    const uint8_t* raw_data() const;
    const ValidityMask& validity() const noexcept { return validity_; }
    const std::shared_ptr<StringHeap>& heap() const noexcept { return heap_; }

    // Makes `out` (a vector of capacity kVectorSize) a read-only view of rows
    // [offset, offset + n). `offset` must be a multiple of kVectorSize and n <= kVectorSize.
    // Rows of `out` at positions >= n are unspecified.
    void Scan(idx_t offset, idx_t n, Vector& out) const;

    size_t MemoryUsage() const noexcept;

    // Number of Scan() calls so far (test/diagnostic counter; relaxed atomic).
    uint64_t scan_calls() const noexcept { return scan_calls_.load(std::memory_order_relaxed); }

  private:
    LogicalType type_;
    idx_t count_;
    std::shared_ptr<Buffer> data_;
    ValidityMask validity_;
    std::shared_ptr<StringHeap> heap_;
    ColumnStats stats_;
    // Precomputed per-vector read-only views, so Scan() performs no allocation.
    std::vector<std::shared_ptr<Buffer>> data_views_;
    std::vector<std::shared_ptr<Buffer>> validity_views_;
    std::shared_ptr<const EncodedColumn> encoded_;
    mutable std::atomic<uint64_t> scan_calls_{0};
};

} // namespace cdb
