#include "execution/group_table.h"

#include <numeric>

namespace cdb {

GroupTable::GroupTable(std::vector<LogicalType> group_types, std::vector<AggregateSpec> aggregates)
    : group_types_(std::move(group_types)), specs_(std::move(aggregates)), index_(group_types_) {
    output_types_ = group_types_;
    for (const AggregateSpec& s : specs_) {
        output_types_.push_back(AggregateResultType(s));
        states_.push_back(MakeAggregateState(s));
    }
    if (group_types_.empty()) {
        DataChunk none;
        none.Initialize({});
        uint32_t id = 0;
        index_.FindOrInsert(none, 1, &id);
        for (auto& s : states_) {
            s->Resize(1);
        }
    }
}

void GroupTable::Sink(const DataChunk& keys, const std::vector<const Vector*>& args, idx_t count) {
    if (count == 0) {
        return;
    }
    CDB_CHECK(args.size() == specs_.size());
    if (group_types_.empty()) { // no GROUP BY: one implicit group, no hashing at all
        for (size_t a = 0; a < states_.size(); a++) {
            states_[a]->UpdateUngrouped(args[a], count);
        }
        return;
    }
    ids_.resize(count);
    index_.FindOrInsert(keys, count, ids_.data());
    for (size_t a = 0; a < states_.size(); a++) {
        states_[a]->Resize(index_.Count());
        states_[a]->Update(ids_.data(), args[a], count);
    }
}

void GroupTable::Combine(const GroupTable& other) {
    const idx_t n = other.GroupCount();
    if (n == 0) {
        return;
    }
    CDB_CHECK(other.group_types_ == group_types_ && other.specs_.size() == specs_.size());
    std::vector<uint32_t> mapping(n, 0);
    if (!group_types_.empty()) {
        idx_t at = 0;
        const ChunkStore& store = other.index_.keys();
        for (idx_t c = 0; c < store.ChunkCount(); c++) {
            const DataChunk& chunk = store.chunk(c);
            index_.FindOrInsert(chunk, chunk.size(), mapping.data() + at);
            at += chunk.size();
        }
    }
    for (size_t a = 0; a < states_.size(); a++) {
        states_[a]->Resize(index_.Count());
        states_[a]->Combine(*other.states_[a], mapping.data(), n);
    }
}

void GroupTable::Scan(idx_t first, idx_t count, DataChunk& out) const {
    CDB_CHECK(first + count <= GroupCount() && count <= kVectorSize);
    out.Reset();
    if (!group_types_.empty()) {
        std::vector<uint32_t> rows(count);
        std::iota(rows.begin(), rows.end(), static_cast<uint32_t>(first));
        for (idx_t c = 0; c < group_types_.size(); c++) {
            index_.keys().Gather(c, rows.data(), count, out.column(c));
        }
    }
    for (size_t a = 0; a < states_.size(); a++) {
        states_[a]->Finalize(first, count, out.column(group_types_.size() + a));
    }
    out.SetCardinality(count);
}

} // namespace cdb
