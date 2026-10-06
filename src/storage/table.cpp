#include "storage/table.h"

#include "common/error.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

namespace {
// True if the row group is empty or a zone map proves that no row can satisfy a filter.
bool CanSkipGroup(const RowGroup& g, const std::vector<TableFilter>& filters) {
    if (g.count() == 0) {
        return true;
    }
    for (const TableFilter& f : filters) {
        if (g.column(f.column_index).stats().CanSkip(f.op, f.constant)) {
            return true;
        }
    }
    return false;
}
} // namespace

bool TableScan::AdvanceToNextGroup() {
    while (next_group_ < snapshot_->row_group_count()) {
        const RowGroup& g = snapshot_->row_group(next_group_++);
        if (CanSkipGroup(g, filters_)) {
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

// ---------------------------------------------------------------- MorselScan

namespace {
std::atomic<idx_t>& MorselRowsSetting() {
    static std::atomic<idx_t> rows{[] {
        const char* env = std::getenv("CDB_MORSEL_ROWS");
        const long long v = env != nullptr ? std::atoll(env) : 0;
        return v > 0 ? AlignUp(static_cast<idx_t>(v), kVectorSize) : idx_t{0};
    }()};
    return rows;
}
} // namespace

idx_t MorselScan::DefaultMorselRows() noexcept {
    const idx_t set = MorselRowsSetting().load(std::memory_order_relaxed);
    return set != 0 ? set : kDefaultMorselRows;
}

void MorselScan::SetDefaultMorselRows(idx_t rows) noexcept {
    MorselRowsSetting().store(rows == 0 ? 0 : AlignUp(rows, kVectorSize),
                              std::memory_order_relaxed);
}

MorselScan::MorselScan(std::shared_ptr<const TableSnapshot> snapshot, std::vector<idx_t> column_ids,
                       std::vector<TableFilter> filters, idx_t morsel_rows, size_t threads)
    : snapshot_(std::move(snapshot)), column_ids_(std::move(column_ids)) {
    const bool adapt =
        morsel_rows == 0 && threads > 1 && MorselRowsSetting().load(std::memory_order_relaxed) == 0;
    if (morsel_rows == 0) {
        morsel_rows = DefaultMorselRows();
    }
    CDB_CHECK(morsel_rows % kVectorSize == 0);
    const auto& schema = snapshot_->schema();
    for (const idx_t c : column_ids_) {
        CDB_CHECK(c < schema.size());
        types_.push_back(schema[c].type);
    }
    for (const TableFilter& f : filters) {
        CDB_CHECK(f.column_index < schema.size());
    }
    std::vector<const RowGroup*> kept;
    idx_t kept_rows = 0;
    for (idx_t g = 0; g < snapshot_->row_group_count(); g++) {
        const RowGroup& group = snapshot_->row_group(g);
        if (CanSkipGroup(group, filters)) {
            skipped_++;
            continue;
        }
        scanned_++;
        kept.push_back(&group);
        kept_rows += group.count();
    }
    if (adapt) {
        const idx_t even = AlignUp(kept_rows / (threads * kMorselsPerThread), kVectorSize);
        morsel_rows = std::clamp<idx_t>(even, kVectorSize, morsel_rows);
    }
    for (const RowGroup* g : kept) {
        const RowGroup& group = *g;
        for (idx_t offset = 0; offset < group.count(); offset += morsel_rows) {
            ScanMorsel m;
            m.group = &group;
            m.offset = offset;
            m.count = std::min(morsel_rows, group.count() - offset);
            m.index = morsels_.size();
            rows_ += m.count;
            morsels_.push_back(m);
        }
    }
}

bool MorselScan::Next(ScanMorsel& morsel) {
    const idx_t i = next_.fetch_add(1, std::memory_order_relaxed);
    if (i >= morsels_.size()) {
        return false;
    }
    morsel = morsels_[i];
    return true;
}

idx_t MorselScan::ReadVector(const ScanMorsel& morsel, idx_t vector_index, DataChunk& out) const {
    CDB_CHECK(out.types() == types_ && out.capacity() == kVectorSize);
    const idx_t first = vector_index * kVectorSize;
    CDB_CHECK(first < morsel.count);
    const idx_t n = std::min(kVectorSize, morsel.count - first);
    for (idx_t i = 0; i < column_ids_.size(); i++) {
        morsel.group->column(column_ids_[i]).Scan(morsel.offset + first, n, out.column(i));
    }
    out.SetCardinality(n);
    return n;
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

void Table::CheckChunkShape(const DataChunk& chunk) const {
    CDB_CHECK(chunk.ColumnCount() == schema_.size());
    for (idx_t c = 0; c < schema_.size(); c++) {
        CDB_CHECK(chunk.types()[c] == schema_[c].type);
    }
}

void Table::ValidateChunk(const DataChunk& chunk) const {
    CheckChunkShape(chunk);
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
}

void Table::AppendRowGroups(std::vector<std::shared_ptr<const RowGroup>> groups) {
    for (const auto& g : groups) {
        CDB_CHECK(g != nullptr && g->ColumnCount() == schema_.size() &&
                  g->count() <= row_group_size_);
    }
    std::unique_lock lock(mutex_);
    if (open_ && open_->count() > 0) {
        sealed_.push_back(open_->Seal()); // so the new groups follow it in order
    }
    open_.reset();
    for (auto& g : groups) {
        sealed_.push_back(std::move(g));
    }
    tail_cache_.reset();
}

void Table::LoadRowGroups(std::vector<std::shared_ptr<const RowGroup>> groups) {
    CDB_CHECK(RowCount() == 0);
    std::shared_ptr<const RowGroup> tail;
    if (!groups.empty() && groups.back()->count() < row_group_size_) {
        tail = std::move(groups.back());
        groups.pop_back();
    }
    AppendRowGroups(std::move(groups));
    if (tail == nullptr) {
        return;
    }
    std::vector<LogicalType> types;
    for (const ColumnDefinition& c : schema_) {
        types.push_back(c.type);
    }
    DataChunk chunk;
    chunk.Initialize(types, kVectorSize);
    for (idx_t at = 0; at < tail->count(); at += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, tail->count() - at);
        chunk.Reset();
        for (idx_t c = 0; c < schema_.size(); c++) {
            tail->column(c).Scan(at, n, chunk.column(c));
        }
        chunk.SetCardinality(n);
        Append(chunk);
    }
}

void Table::Append(const DataChunk& chunk) {
    CheckChunkShape(chunk);
    if (chunk.size() == 0) {
        return;
    }
    // Enforce NOT NULL up front so a rejected chunk leaves the table untouched.
    ValidateChunk(chunk);
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
