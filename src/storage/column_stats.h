#pragma once

#include "common/types.h"
#include "storage/hyperloglog.h"
#include "types/value.h"
#include "vector/validity_mask.h"

#include <memory>
#include <optional>

namespace cdb {

enum class CompareOp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

const char* CompareOpName(CompareOp op) noexcept;

// Zone-map statistics for one column segment: row/NULL counts and exact min/max over the
// non-NULL values. All ordering follows Value::Compare (so DOUBLE uses the total order with NaN
// last, matching how the engine's comparison kernels order doubles).
//
// Bounds are unavailable (nullopt) when every row is NULL, or for VARCHAR segments containing a
// string longer than kMaxBoundStringLength (truncated bounds are not worth the complexity; such
// segments are simply never pruned).
struct ColumnStats {
    static constexpr size_t kMaxBoundStringLength = 64;

    idx_t count = 0;
    idx_t null_count = 0;
    std::optional<Value> min;
    std::optional<Value> max;
    // A sketch of the distinct non-NULL values, for estimating how many a column has. Present on
    // sealed segments; null for the frozen copy of an open tail (computed on demand when the
    // optimizer asks for table statistics).
    std::shared_ptr<const HyperLogLog> distinct;

    bool AllNull() const noexcept { return null_count == count; }

    // True if NO row of the segment can satisfy `column <op> constant`; i.e. the segment may
    // safely be skipped by a scan that only needs matching rows. NULL never satisfies a
    // comparison, so an all-NULL segment (or a NULL constant) is always skippable. A `false`
    // answer means "maybe": pruning is sound but not complete.
    bool CanSkip(CompareOp op, const Value& constant) const;

    bool CanSkipIsNull() const noexcept { return null_count == 0; }
    bool CanSkipIsNotNull() const noexcept { return null_count == count; }
};

// Computes statistics over the first `count` rows of a contiguous element array (not the distinct
// sketch: see ComputeDistinctSketch).
ColumnStats ComputeColumnStats(LogicalType type, const uint8_t* data, const ValidityMask& validity,
                               idx_t count);

// Shows the non-NULL values among the first `count` rows of a contiguous element array to
// `sketch`. Equal values hash equally (-0.0 and 0.0, every NaN), so a value counts once.
void AddToDistinctSketch(HyperLogLog& sketch, LogicalType type, const uint8_t* data,
                         const ValidityMask& validity, idx_t count);

std::shared_ptr<const HyperLogLog> ComputeDistinctSketch(LogicalType type, const uint8_t* data,
                                                         const ValidityMask& validity, idx_t count);

} // namespace cdb
