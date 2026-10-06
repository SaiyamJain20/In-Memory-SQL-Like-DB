#pragma once

#include "common/types.h"
#include "storage/column_definition.h"
#include "storage/row_group.h"

#include <atomic>
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

// A run of consecutive rows of one row group: the unit of parallel scanning ("morsel"). Its rows
// are read one vector at a time with MorselScan::ReadVector().
struct ScanMorsel {
    const RowGroup* group = nullptr;
    idx_t offset = 0; // first row within the group (a multiple of kVectorSize)
    idx_t count = 0;  // rows in the morsel
    idx_t index = 0;  // position in scan order: 0, 1, 2, ... over the whole scan
};

// The shared half of a parallel scan. Row groups that the zone maps rule out are dropped, the rest
// are cut into morsels of up to `morsel_rows` rows, and any number of threads claim morsels with
// Next() (an atomic cursor, so each is handed out exactly once) and read them with ReadVector().
// A single thread consuming morsels in order sees exactly the chunks TableScan produces.
class MorselScan {
  public:
    static constexpr idx_t kDefaultMorselRows = 8 * kVectorSize;

    // The morsel size used when none is given: kDefaultMorselRows, or CDB_MORSEL_ROWS from the
    // environment, or whatever SetDefaultMorselRows() last set (tests use one vector to force many
    // morsels, and so many interleavings, on small tables).
    static idx_t DefaultMorselRows() noexcept;
    static void SetDefaultMorselRows(idx_t rows) noexcept; // 0 restores the built-in default

    // Morsels per thread a scan aims for when it picks the morsel size itself, so that a small
    // table still keeps every thread busy and the last morsels even out the finish.
    static constexpr idx_t kMorselsPerThread = 4;

    // `morsel_rows` must be a positive multiple of kVectorSize; 0 means DefaultMorselRows(). When
    // it is 0, nothing set the default explicitly (SetDefaultMorselRows, CDB_MORSEL_ROWS) and more
    // than one `threads` will read the scan, the morsel size shrinks (never below one vector) so
    // that there are about kMorselsPerThread morsels per thread: a 15,000-row table is then eight
    // morsels, not one that a single thread must read while the rest of the pool idles.
    MorselScan(std::shared_ptr<const TableSnapshot> snapshot, std::vector<idx_t> column_ids,
               std::vector<TableFilter> filters = {}, idx_t morsel_rows = 0, size_t threads = 1);

    const std::vector<LogicalType>& types() const noexcept { return types_; }
    idx_t MorselCount() const noexcept { return morsels_.size(); }
    idx_t RowCount() const noexcept { return rows_; } // rows in the morsels (after pruning)
    idx_t row_groups_scanned() const noexcept { return scanned_; }
    idx_t row_groups_skipped() const noexcept { return skipped_; }

    // Claims the next unclaimed morsel; false when all are taken. Thread-safe.
    bool Next(ScanMorsel& morsel);

    // The number of vectors the morsel spans.
    static idx_t VectorCount(const ScanMorsel& morsel) noexcept {
        return (morsel.count + kVectorSize - 1) / kVectorSize;
    }

    // Fills `out` (Initialize()d with types()) with vector `vector_index` of the morsel and returns
    // its row count (<= kVectorSize). The vectors are zero-copy views of the stored segments,
    // exactly as with TableScan. Thread-safe: reads only immutable data.
    idx_t ReadVector(const ScanMorsel& morsel, idx_t vector_index, DataChunk& out) const;

  private:
    std::shared_ptr<const TableSnapshot> snapshot_; // keeps the row groups alive
    std::vector<idx_t> column_ids_;
    std::vector<LogicalType> types_;
    std::vector<ScanMorsel> morsels_;
    idx_t rows_ = 0;
    idx_t scanned_ = 0;
    idx_t skipped_ = 0;
    std::atomic<idx_t> next_{0};
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

    // Throws the error Append() would throw for a NULL in a NOT NULL column of `chunk`, without
    // appending anything.
    void ValidateChunk(const DataChunk& chunk) const;

    // Appends row groups that were built and sealed elsewhere (each with this table's schema and at
    // most row_group_size() rows), in order, after the existing rows: a still-open tail is sealed
    // as a short group first. For loaders that build row groups on several threads. The groups
    // are not checked against NOT NULL (see ValidateChunk).
    void AppendRowGroups(std::vector<std::shared_ptr<const RowGroup>> groups);

    // Fills an empty table from row groups read back from a checkpoint, in order. Groups become
    // sealed row groups as they are, except that a last group with fewer than row_group_size()
    // rows is re-opened as the table's tail so that later appends continue it (restarting a
    // database many times must not leave a short row group behind each time). NOT NULL columns
    // are checked in the re-opened tail only: the sealed groups are trusted, they were written by
    // a table that enforced it.
    void LoadRowGroups(std::vector<std::shared_ptr<const RowGroup>> groups);

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
    void CheckChunkShape(const DataChunk& chunk) const; // aborts on a column count / type mismatch

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
