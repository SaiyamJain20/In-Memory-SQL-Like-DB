#pragma once

#include "vector/data_chunk.h"

#include <vector>

namespace cdb {

// An append-only, row-addressable collection of rows held as flat DataChunks of kVectorSize rows
// (the last one partially filled). Used wherever an operator has to remember rows: group keys,
// join build sides, sort buffers. Row `r` lives in chunk r / kVectorSize at offset r % kVectorSize.
class ChunkStore {
  public:
    static constexpr idx_t kShift = 11;
    static_assert((idx_t{1} << kShift) == kVectorSize);

    explicit ChunkStore(std::vector<LogicalType> types);
    ChunkStore(ChunkStore&&) noexcept = default;
    ChunkStore& operator=(ChunkStore&&) noexcept = default;
    ChunkStore(const ChunkStore&) = delete;
    ChunkStore& operator=(const ChunkStore&) = delete;

    const std::vector<LogicalType>& types() const noexcept { return types_; }
    idx_t ColumnCount() const noexcept { return types_.size(); }
    idx_t Count() const noexcept { return count_; }

    // Appends logical rows sel[0..count) of `chunk` (or rows 0..count-1 when sel is null); the
    // chunk's column types must equal types(). Out-of-line strings are copied.
    void Append(const DataChunk& chunk, const SelectionVector* sel, idx_t count);
    void Append(const DataChunk& chunk) { Append(chunk, nullptr, chunk.size()); }

    // Appends every row of `other` (same types).
    void AppendStore(const ChunkStore& other);

    // out[i] = column `column` of row rows[i], for i in [0, n). `out` is a Flat vector of the
    // column's type with capacity >= n.
    void Gather(idx_t column, const uint32_t* rows, idx_t n, Vector& out) const;

    // Rows [first, first + n) of every column, in order, into `out` (n <= kVectorSize; `out`
    // must be Initialize()d with types()).
    void ReadRange(idx_t first, idx_t n, DataChunk& out) const;

    // The chunks, for kernels that walk the stored data directly.
    idx_t ChunkCount() const noexcept { return chunks_.size(); }
    const DataChunk& chunk(idx_t i) const { return chunks_[i]; }

    Value GetValue(idx_t column, idx_t row) const {
        return chunks_[row >> kShift].GetValue(column, row & (kVectorSize - 1));
    }

    void Clear();

  private:
    DataChunk& TailWithRoom();

    std::vector<LogicalType> types_;
    std::vector<DataChunk> chunks_;
    idx_t count_ = 0;
};

} // namespace cdb
