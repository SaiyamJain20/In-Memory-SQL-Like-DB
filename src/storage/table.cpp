#include "storage/table.h"

#include "common/error.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace cdb {

namespace {

std::string Lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

// ---------------------------------------------------------------- TableSnapshot

TableSnapshot::TableSnapshot(std::vector<ColumnDefinition> schema,
                             std::vector<std::shared_ptr<const RowGroup>> groups)
    : schema_(std::move(schema)), groups_(std::move(groups)) {
    for (const auto& g : groups_) {
        CDB_CHECK(g->ColumnCount() == schema_.size());
        row_count_ += g->count();
    }
}

// ---------------------------------------------------------------- TableScan

TableScan::TableScan(std::shared_ptr<const TableSnapshot> snapshot, std::vector<idx_t> column_ids,
                     std::vector<TableFilter> filters)
    : snapshot_(std::move(snapshot)), column_ids_(std::move(column_ids)),
      filters_(std::move(filters)) {
    const auto& schema = snapshot_->schema();
    for (idx_t c : column_ids_) {
        CDB_CHECK(c < schema.size());
        types_.push_back(schema[c].type);
    }
    for (const TableFilter& f : filters_) {
        CDB_CHECK(f.column_index < schema.size());
    }
}

bool TableScan::AdvanceToNextGroup() {
    while (next_group_ < snapshot_->row_group_count()) {
        const RowGroup& g = snapshot_->row_group(next_group_++);
        bool skip = g.count() == 0;
        for (const TableFilter& f : filters_) {
            if (skip) {
                break;
            }
            skip = g.column(f.column_index).stats().CanSkip(f.op, f.constant);
        }
        if (skip) {
            skipped_++;
            continue;
        }
        scanned_++;
        group_ = &g;
        offset_ = 0;
        return true;
    }
    group_ = nullptr;
    return false;
}

bool TableScan::Next(DataChunk& out) {
    CDB_CHECK(out.types() == types_ && out.capacity() == kVectorSize);
    while (group_ == nullptr || offset_ >= group_->count()) {
        if (!AdvanceToNextGroup()) {
            out.SetCardinality(0);
            return false;
        }
    }
    const idx_t n = std::min(kVectorSize, group_->count() - offset_);
    for (idx_t i = 0; i < column_ids_.size(); i++) {
        group_->column(column_ids_[i]).Scan(offset_, n, out.column(i));
    }
    out.SetCardinality(n);
    offset_ += n;
    return true;
}

// ---------------------------------------------------------------- Table

Table::Table(std::string name, std::vector<ColumnDefinition> schema, idx_t row_group_size)
    : name_(std::move(name)), schema_(std::move(schema)), row_group_size_(row_group_size) {
    CDB_CHECK(row_group_size_ >= kVectorSize && row_group_size_ % kVectorSize == 0);
    if (schema_.empty()) {
        throw Error(ErrorCode::Binder, "table \"" + name_ + "\" must have at least one column");
    }
    std::unordered_set<std::string> seen;
    for (const ColumnDefinition& c : schema_) {
        if (c.name.empty()) {
            throw Error(ErrorCode::Binder, "column names must not be empty");
        }
        if (!seen.insert(Lower(c.name)).second) {
            throw Error(ErrorCode::Binder, "duplicate column name \"" + c.name + "\"");
        }
    }
}

std::optional<idx_t> Table::FindColumn(const std::string& name) const {
    const std::string wanted = Lower(name);
    for (idx_t i = 0; i < schema_.size(); i++) {
        if (Lower(schema_[i].name) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

void Table::Append(const DataChunk& chunk) {
    CDB_CHECK(chunk.ColumnCount() == schema_.size());
    for (idx_t c = 0; c < schema_.size(); c++) {
        CDB_CHECK(chunk.types()[c] == schema_[c].type);
    }
    if (chunk.size() == 0) {
        return;
    }
    // Enforce NOT NULL up front so a rejected chunk leaves the table untouched.
    for (idx_t c = 0; c < schema_.size(); c++) {
        if (!schema_[c].not_null) {
            continue;
        }
        UnifiedFormat u;
        chunk.column(c).ToUnified(u);
        for (idx_t r = 0; r < chunk.size(); r++) {
            if (!u.IsValid(r)) {
                throw Error(ErrorCode::Execution, "NOT NULL constraint failed: column \"" +
                                                      schema_[c].name + "\" of table \"" + name_ +
                                                      "\"");
            }
        }
    }
    std::unique_lock lock(mutex_);
    idx_t pos = 0;
    while (pos < chunk.size()) {
        if (!open_) {
            open_ = std::make_unique<RowGroupBuilder>(schema_, row_group_size_);
        }
        const idx_t n = std::min(open_->max_rows() - open_->count(), chunk.size() - pos);
        open_->Append(chunk, pos, n);
        pos += n;
        if (open_->full()) {
            sealed_.push_back(open_->Seal());
            open_.reset();
        }
    }
    tail_cache_.reset(); // safe: we hold the exclusive lock, so no Snapshot() is running
}

void Table::Merge(std::unique_ptr<Table> staging) {
    CDB_CHECK(staging != nullptr && staging.get() != this);
    CDB_CHECK(staging->row_group_size_ == row_group_size_ &&
              staging->schema_.size() == schema_.size());
    for (idx_t c = 0; c < schema_.size(); c++) {
        CDB_CHECK(staging->schema_[c].type == schema_[c].type);
    }
    std::unique_lock lock(mutex_);
    // Seal our partial tail as a (short) row group so the new rows follow it in order.
    if (open_ && open_->count() > 0) {
        sealed_.push_back(open_->Seal());
    }
    open_.reset();
    for (auto& group : staging->sealed_) {
        sealed_.push_back(std::move(group));
    }
    open_ = std::move(staging->open_);
    tail_cache_.reset();
}

idx_t Table::RowCount() const {
    std::shared_lock lock(mutex_);
    idx_t rows = open_ ? open_->count() : 0;
    for (const auto& g : sealed_) {
        rows += g->count();
    }
    return rows;
}

size_t Table::MemoryUsage() const {
    auto snap = Snapshot();
    size_t bytes = 0;
    for (idx_t i = 0; i < snap->row_group_count(); i++) {
        bytes += snap->row_group(i).MemoryUsage();
    }
    return bytes;
}

std::shared_ptr<const TableSnapshot> Table::Snapshot() const {
    std::shared_lock lock(mutex_);
    std::vector<std::shared_ptr<const RowGroup>> groups = sealed_;
    if (open_ && open_->count() > 0) {
        std::lock_guard<std::mutex> guard(tail_mutex_);
        if (!tail_cache_) {
            tail_cache_ = open_->Snapshot();
        }
        groups.push_back(tail_cache_);
    }
    return std::make_shared<TableSnapshot>(schema_, std::move(groups));
}

} // namespace cdb
