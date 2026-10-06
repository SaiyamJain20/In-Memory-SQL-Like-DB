#pragma once

#include "storage/binary_io.h"
#include "storage/column_segment.h"
#include "vector/data_chunk.h"

#include <memory>
#include <vector>

namespace cdb {

// The byte formats of column data, shared by the checkpoint file (whole column segments, in their
// stored encoding) and the write-ahead log (chunks of rows).
//
// A column of `n` rows is written as
//     u8 has_nulls | (has_nulls: ceil(n / 8) bytes, bit i set = row i is valid) | the values of the
//     valid rows only: BOOLEAN 1 byte (0 or 1), INTEGER / DATE 4, BIGINT / DOUBLE 8 (the bit
//     pattern), VARCHAR u32 length + bytes
// so a NULL costs one bit and its (meaningless) value nothing, and the bytes do not depend on what
// was left in the storage of a NULL row. Every reader here treats its input as hostile: counts and
// lengths are checked against what remains, and a problem throws Error(ErrorCode::Corruption).

// ---- single values (zone-map bounds) -------------------------------------------------------
void WriteValue(BinaryWriter& w, const Value& value); // value must not be NULL
Value ReadValue(BinaryReader& r, LogicalType type);

// ---- one column of rows --------------------------------------------------------------------
// Writes `n` rows read through `sel` (null: rows 0..n-1) from the element array `data`.
void WriteColumnData(BinaryWriter& w, LogicalType type, const uint8_t* data, const sel_t* sel,
                     const ValidityMask& validity, idx_t n);
// Reads what WriteColumnData wrote into `data` (room for n elements, zeroed) and `validity`
// (capacity >= n, all valid on entry); VARCHAR bytes are copied into `heap`. Returns the number of
// NULL rows.
idx_t ReadColumnData(BinaryReader& r, LogicalType type, idx_t n, uint8_t* data,
                     ValidityMask& validity, StringHeap* heap);

// The validity half alone, for encoded segments.
void WriteValidity(BinaryWriter& w, const ValidityMask& validity, idx_t n);
idx_t ReadValidity(BinaryReader& r, idx_t n, ValidityMask& validity);

// ---- a chunk of rows (write-ahead log) -----------------------------------------------------
void WriteChunk(BinaryWriter& w, const DataChunk& chunk);
// Reads one chunk of `types` into `out` (Initialized here).
void ReadChunk(BinaryReader& r, const std::vector<LogicalType>& types, DataChunk& out);

// ---- a column segment (checkpoint file) ----------------------------------------------------
// Type, row count, zone-map statistics, validity and the values: raw, or in the segment's encoding.
void WriteSegment(BinaryWriter& w, const ColumnSegment& segment);
// Reads a segment that must be of `type` with `count` rows. The statistics are read, not
// recomputed, but checked against the validity bits they summarise.
std::shared_ptr<ColumnSegment> ReadSegment(BinaryReader& r, LogicalType type, idx_t count);

} // namespace cdb
