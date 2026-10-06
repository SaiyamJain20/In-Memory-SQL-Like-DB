#pragma once

#include "planner/bound_expression.h"
#include "vector/vector.h"

#include <memory>

namespace cdb {

struct AggregateSpec {
    AggregateKind kind = AggregateKind::CountStar;
    LogicalType arg_type = LogicalType::Integer(); // ignored for COUNT(*)
    bool distinct = false;
};

// The type an aggregate produces: COUNT -> BIGINT, SUM -> BIGINT (integers) or DOUBLE, AVG ->
// DOUBLE, MIN/MAX -> the argument type. Matches what the binder assigns.
LogicalType AggregateResultType(const AggregateSpec& spec);

// The state of one aggregate function for every group, kept as flat arrays indexed by group id.
// Updates are batched ("row i of this vector belongs to group g[i]") so the inner loops are
// tight, and states can be merged (Combine), which is what lets per-thread tables be combined.
// NULL arguments are ignored, as in SQL.
class AggregateState {
  public:
    virtual ~AggregateState() = default;

    // Makes room for groups [0, groups); new groups start empty.
    virtual void Resize(idx_t groups) = 0;

    // Row i (< count) of `arg` (null for COUNT(*); any vector format) updates group groups[i].
    virtual void Update(const uint32_t* groups, const Vector* arg, idx_t count) = 0;

    // Merges group s of `src` (a state of the same spec) into group dst_groups[s], for every
    // s < src_groups. The destination must already be Resize()d to cover the targets.
    virtual void Combine(const AggregateState& src, const uint32_t* dst_groups,
                         idx_t src_groups) = 0;

    // Writes the results of groups [first, first + count) to rows 0.. of the Flat vector `out`
    // (of AggregateResultType). Groups that saw no non-NULL input give NULL (except COUNT: 0).
    virtual void Finalize(idx_t first, idx_t count, Vector& out) const = 0;
};

std::unique_ptr<AggregateState> MakeAggregateState(const AggregateSpec& spec);

} // namespace cdb
