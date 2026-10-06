#pragma once

#include "execution/aggregate_state.h"
#include "execution/key_index.h"

#include <memory>
#include <vector>

namespace cdb {

// The hash table behind GROUP BY and DISTINCT: a KeyIndex over the group-by key tuples plus one
// AggregateState per aggregate, all indexed by group id (groups are numbered in order of first
// appearance, which makes results deterministic). With no group columns there is exactly one
// group, which exists from the start, so a global aggregate over empty input still yields a row.
class GroupTable {
  public:
    GroupTable(std::vector<LogicalType> group_types, std::vector<AggregateSpec> aggregates);

    idx_t GroupCount() const noexcept { return index_.Count(); }
    idx_t GroupColumnCount() const noexcept { return group_types_.size(); }
    const std::vector<LogicalType>& output_types() const noexcept { return output_types_; }

    // Adds rows [0, count). `keys` has one column per group type; args[i] is aggregate i's
    // argument vector (null for COUNT(*)).
    void Sink(const DataChunk& keys, const std::vector<const Vector*>& args, idx_t count);

    // Merges every group of `other` (same shape) into this table.
    void Combine(const GroupTable& other);

    // Merges only the groups groups[0..n) of `other` (same shape; unchanged) into this table, so
    // that the groups of one hash partition of several tables can be merged independently of the
    // other partitions. Only if CanCombineGroups().
    void CombineGroups(const GroupTable& other, const uint32_t* groups, idx_t n);
    // False when an aggregate is DISTINCT: those states merge whole tables only.
    bool CanCombineGroups() const;
    // The hash of the key of group `id` (grouped tables only).
    uint64_t GroupHash(idx_t id) const noexcept { return index_.Hash(id); }

    // Groups [first, first + count) as rows of `out` (Initialize()d with output_types(); count
    // <= kVectorSize): the key columns, then one column per aggregate.
    void Scan(idx_t first, idx_t count, DataChunk& out) const;

  private:
    std::vector<LogicalType> group_types_;
    std::vector<AggregateSpec> specs_;
    std::vector<LogicalType> output_types_;
    KeyIndex index_;
    std::vector<std::unique_ptr<AggregateState>> states_;
    std::vector<uint32_t> ids_;
};

} // namespace cdb
