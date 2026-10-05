#pragma once

#include "common/types.h"
#include "storage/column_definition.h"
#include "storage/row_group.h"

#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace cdb {

// A pruning hint pushed into a scan: `column <op> constant`. Row groups whose zone map proves no
// row can match are skipped entirely. Rows inside the groups that ARE scanned are not filtered;
// the Filter operator still applies the real predicate.
struct TableFilter {
    idx_t column_index; // index into the table schema (not into the projection)
    CompareOp op;
    Value constant;
};

// A consistent, immutable view of a table: its sealed row groups plus a frozen copy of the
// still-open tail. Cheap to hold, safe to scan from many threads, and unaffected by later appends.
class TableSnapshot {
  public:
    TableSnapshot(std::vector<ColumnDefinition> schema,
                  std::vector<std::shared_ptr<const RowGroup>> groups);

    const std::vector<ColumnDefinition>& schema() const noexcept { return schema_; }
    idx_t row_count() const noexcept { return row_count_; }
    idx_t row_group_count() const noexcept { return groups_.size(); }
    const RowGroup& row_group(idx_t i) const { return *groups_.at(i); }
    const std::shared_ptr<const RowGroup>& row_group_ptr(idx_t i) const { return groups_.at(i); }

  private:
    std::vector<ColumnDefinition> schema_;
    std::vector<std::shared_ptr<const RowGroup>> groups_;
    idx_t row_count_ = 0;
};

// Sequential cursor over a snapshot, one DataChunk (<= kVectorSize rows) at a time. Output
// vectors are zero-copy, read-only views of the stored segments: valid as long as the chunk holds
// them, and must not be modified.
class TableScan {
  public:
    // `column_ids` selects and orders the output columns (indices into the table schema).
    TableScan(std::shared_ptr<const TableSnapshot> snapshot, std::vector<idx_t> column_ids,
              std::vector<TableFilter> filters = {});

    // Types of the output columns; Initialize the chunk passed to Next() with these.
    const std::vector<LogicalType>& types() const noexcept { return types_; }

    // Fills `out` with the next rows and returns true, or returns false (and sets the chunk's
    // size to 0) when the scan is exhausted.
    bool Next(DataChunk& out);

    idx_t row_groups_scanned() const noexcept { return scanned_; }
    idx_t row_groups_skipped() const noexcept { return skipped_; }

  private:
    bool AdvanceToNextGroup();

    std::shared_ptr<const TableSnapshot> snapshot_;
    std::vector<idx_t> column_ids_;
    std::vector<TableFilter> filters_;
    std::vector<LogicalType> types_;
    idx_t next_group_ = 0;
    const RowGroup* group_ = nullptr;
    idx_t offset_ = 0;
    idx_t scanned_ = 0;
    idx_t skipped_ = 0;
};

// An append-only columnar table. Writers append whole DataChunks; readers take snapshots and
// scan them without holding any lock. Thread-safe.
class Table {
  public:
    // `row_group_size` must be a positive multiple of kVectorSize (tests use small groups to
    // exercise many boundaries cheaply).
    Table(std::string name, std::vector<ColumnDefinition> schema,
          idx_t row_group_size = kRowGroupSize);

    const std::string& name() const noexcept { return name_; }
    const std::vector<ColumnDefinition>& schema() const noexcept { return schema_; }
    idx_t row_group_size() const noexcept { return row_group_size_; }

    // Case-insensitive column lookup.
    std::optional<idx_t> FindColumn(const std::string& name) const;

    // Appends all rows of `chunk`, whose column types must equal the schema. The chunk's vectors
    // may be in any format.
    void Append(const DataChunk& chunk);

    // Atomically appends every row of `staging` to this table by moving its row groups in (no
    // data is copied) and adopting its still-open tail. Used for bulk loads: build the data in a
    // private staging table, then publish it all at once, so a failed load leaves no trace and
    // concurrent readers see either none or all of the new rows. `staging` must have the same
    // column types and row group size; it is consumed.
    void Merge(std::unique_ptr<Table> staging);

    idx_t RowCount() const;
    size_t MemoryUsage() const;

    std::shared_ptr<const TableSnapshot> Snapshot() const;

    TableScan Scan(std::vector<idx_t> column_ids, std::vector<TableFilter> filters = {}) const {
        return TableScan(Snapshot(), std::move(column_ids), std::move(filters));
    }

  private:
    std::string name_;
    std::vector<ColumnDefinition> schema_;
    idx_t row_group_size_;

    mutable std::shared_mutex mutex_;
    std::vector<std::shared_ptr<const RowGroup>> sealed_;
    std::unique_ptr<RowGroupBuilder> open_;

    // Frozen copy of `open_`, reused by successive Snapshot() calls until the next Append.
    mutable std::mutex tail_mutex_;
    mutable std::shared_ptr<const RowGroup> tail_cache_;
};

} // namespace cdb
