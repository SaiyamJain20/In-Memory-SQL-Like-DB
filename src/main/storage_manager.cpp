#include "main/storage_manager.h"

#include "storage/checkpoint.h"
#include "storage/segment_io.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace cdb {

namespace {

enum class Op : uint8_t { CreateTable = 1, DropTable = 2, Append = 3 };

constexpr const char* kCheckpointPrefix = "checkpoint-";
constexpr const char* kCheckpointSuffix = ".cdb";
constexpr const char* kWalPrefix = "wal-";
constexpr const char* kWalSuffix = ".log";
constexpr const char* kTmpSuffix = ".tmp";

std::string Hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool StartsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool EndsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

// "<prefix><16 hex digits><suffix>" -> the epoch.
bool ParseEpoch(const std::string& name, const char* prefix, const char* suffix, uint64_t& epoch) {
    const std::string p = prefix, s = suffix;
    if (!StartsWith(name, p) || !EndsWith(name, s) || name.size() != p.size() + 16 + s.size()) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < 16; i++) {
        const char c = name[p.size() + i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else {
            return false;
        }
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    epoch = v;
    return true;
}

[[noreturn]] void Corrupt(const std::string& what) {
    throw Error(ErrorCode::Corruption, what);
}

void WriteTableDefinition(BinaryWriter& w, const Table& table) {
    w.String(table.name());
    w.U32(static_cast<uint32_t>(table.schema().size()));
    for (const ColumnDefinition& c : table.schema()) {
        w.String(c.name);
        w.U8(static_cast<uint8_t>(c.type.id()));
        w.U8(c.not_null ? 1 : 0);
    }
    w.U64(table.row_group_size());
}

} // namespace

std::string StorageManager::CheckpointName(uint64_t epoch) {
    return kCheckpointPrefix + Hex16(epoch) + kCheckpointSuffix;
}
std::string StorageManager::WalName(uint64_t epoch) {
    return kWalPrefix + Hex16(epoch) + kWalSuffix;
}

StorageManager::StorageManager(Catalog& catalog, std::string dir, StorageOptions options,
                               SchedulerProvider scheduler)
    : catalog_(catalog), dir_(std::move(dir)), options_(std::move(options)),
      scheduler_(std::move(scheduler)),
      fs_(options_.fs != nullptr ? options_.fs : PosixFileSystem()) {
    Recover();
}

StorageManager::~StorageManager() {
    try {
        if (failed_.load()) {
            return;
        }
        if (options_.checkpoint_on_close) {
            Checkpoint();
        } else if (wal_ != nullptr) {
            wal_->Sync();
        }
    } catch (...) { // a destructor cannot report; the log is intact and recovery will replay it
    }
}

uint64_t StorageManager::uncheckpointed_bytes() const {
    const std::lock_guard<std::mutex> lock(commit_mutex_);
    return bytes_uncheckpointed_;
}

std::string StorageManager::last_checkpoint_error() const {
    const std::lock_guard<std::mutex> lock(error_mutex_);
    return last_checkpoint_error_;
}

// ---------------------------------------------------------------------------------- recovery

void StorageManager::Recover() {
    fs_->CreateDirectories(dir_);
    lock_ = fs_->Lock(JoinPath(dir_, "LOCK"));

    std::map<uint64_t, std::string> checkpoints, wals;
    for (const std::string& name : fs_->List(dir_)) {
        uint64_t epoch;
        if (ParseEpoch(name, kCheckpointPrefix, kCheckpointSuffix, epoch)) {
            checkpoints[epoch] = name;
        } else if (ParseEpoch(name, kWalPrefix, kWalSuffix, epoch)) {
            wals[epoch] = name;
        }
    }

    if (checkpoints.empty() && wals.empty()) { // nothing here: a new database
        wal_ = WalWriter::Create(*fs_, JoinPath(dir_, WalName(0)), 0);
        fs_->SyncDirectory(dir_);
        recovery_.created = true;
        RemoveObsoleteFiles(0, true); // an unfinished checkpoint of a database that never got far
        return;
    }

    // The newest checkpoint is the base. If it does not verify that is an error: it was renamed
    // into place only after an fsync, so it cannot be a torn write, and falling back to an older
    // state could silently drop committed data.
    uint64_t base = 0;
    if (!checkpoints.empty()) {
        base = checkpoints.rbegin()->first;
        LoadCheckpoint(JoinPath(dir_, checkpoints.rbegin()->second));
        recovery_.had_checkpoint = true;
        recovery_.checkpoint_epoch = base;
        checkpoint_epoch_ = base;
    }

    // The logs from the base on must be there, all of them: one per epoch, no gaps.
    std::vector<uint64_t> chain;
    for (const auto& [epoch, name] : wals) {
        if (epoch >= base) {
            chain.push_back(epoch);
        }
    }
    if (chain.empty() || chain.front() != base) {
        Corrupt("the write-ahead log " + WalName(base) + " is missing");
    }
    for (size_t i = 0; i < chain.size(); i++) {
        if (chain[i] != base + i) {
            Corrupt("the write-ahead log " + WalName(base + i) + " is missing (found " +
                    WalName(chain[i]) + ")");
        }
    }

    for (size_t i = 0; i < chain.size(); i++) {
        const uint64_t epoch = chain[i];
        const std::string path = JoinPath(dir_, WalName(epoch));
        const bool newest = i + 1 == chain.size();
        const WalScan scan = ScanWal(*fs_, path, epoch);
        if (!newest && (!scan.header_valid || scan.discarded_bytes() != 0)) {
            // an older log was fsynced completely before the next one was started
            Corrupt("the write-ahead log " + WalName(epoch) +
                    " is damaged, but a later log exists");
        }
        ReplayWal(*fs_, path, scan, [&](const uint8_t* data, size_t size) {
            ApplyPayload(data, size);
            recovery_.frames++;
        });
        recovery_.wal_files++;
        recovery_.transactions += scan.committed_transactions;
        if (scan.header_valid) {
            bytes_uncheckpointed_ += scan.valid_size - kWalHeaderSize;
        }
        if (newest) {
            recovery_.discarded_bytes = scan.discarded_bytes();
            recovery_.discarded_frames = scan.discarded_frames;
            wal_ = WalWriter::Continue(*fs_, path, epoch, scan.valid_size, scan.next_sequence);
            wal_epoch_ = epoch;
        }
    }
    transactions_total_ = recovery_.transactions;
    RemoveObsoleteFiles(base, true);
}

void StorageManager::LoadCheckpoint(const std::string& path) {
    const std::shared_ptr<TaskScheduler> pool = Scheduler();
    CheckpointImage image = ReadCheckpoint(*fs_, path, pool.get());
    for (TableImage& t : image.tables) {
        try {
            auto table = std::make_shared<Table>(t.name, t.schema, t.row_group_size);
            table->LoadRowGroups(std::move(t.groups));
            catalog_.AddTable(std::move(table));
        } catch (const Error& e) {
            if (e.code() == ErrorCode::Corruption) {
                throw;
            }
            Corrupt("checkpoint '" + path + "', table '" + t.name + "': " + e.what());
        }
    }
    recovery_.tables = image.tables.size();
}

void StorageManager::ApplyPayload(const uint8_t* data, size_t size) {
    BinaryReader r(data, size, "write-ahead log record");
    try {
        while (!r.AtEnd()) {
            const uint8_t op = r.U8();
            switch (static_cast<Op>(op)) {
            case Op::CreateTable: {
                std::string name(r.String());
                const uint32_t ncols = r.Count(6);
                std::vector<ColumnDefinition> schema;
                for (uint32_t c = 0; c < ncols; c++) {
                    std::string column(r.String());
                    const uint8_t type = r.U8();
                    const uint8_t not_null = r.U8();
                    if (type > static_cast<uint8_t>(TypeId::Varchar) || not_null > 1) {
                        r.Fail("an invalid column definition");
                    }
                    schema.push_back(ColumnDefinition{
                        std::move(column), LogicalType(static_cast<TypeId>(type)), not_null != 0});
                }
                const uint64_t row_group_size = r.U64();
                if (row_group_size < kVectorSize || row_group_size % kVectorSize != 0 ||
                    row_group_size > (idx_t{1} << 26)) {
                    r.Fail("a row group size of " + std::to_string(row_group_size));
                }
                catalog_.AddTable(std::make_shared<Table>(name, std::move(schema), row_group_size));
                break;
            }
            case Op::DropTable: {
                const std::string name(r.String());
                catalog_.DropTable(name, false);
                break;
            }
            case Op::Append: {
                const std::string name(r.String());
                const std::shared_ptr<Table> table = catalog_.TryGetTable(name);
                if (table == nullptr) {
                    r.Fail("an append to the unknown table '" + name + "'");
                }
                std::vector<LogicalType> types;
                for (const ColumnDefinition& c : table->schema()) {
                    types.push_back(c.type);
                }
                DataChunk chunk;
                ReadChunk(r, types, chunk);
                table->Append(chunk);
                break;
            }
            default:
                r.Fail("an unknown operation " + std::to_string(op));
            }
        }
    } catch (const Error& e) {
        if (e.code() == ErrorCode::Corruption) {
            throw;
        }
        // e.g. creating a table that exists, or a NULL in a NOT NULL column: the log is not
        // something this database could have written
        Corrupt(std::string("write-ahead log replay: ") + e.what());
    }
}

void StorageManager::RemoveObsoleteFiles(uint64_t keep_from_epoch, bool include_tmp) {
    try {
        bool removed = false;
        for (const std::string& name : fs_->List(dir_)) {
            uint64_t epoch;
            const bool old_checkpoint =
                ParseEpoch(name, kCheckpointPrefix, kCheckpointSuffix, epoch) &&
                epoch < keep_from_epoch;
            const bool old_wal =
                ParseEpoch(name, kWalPrefix, kWalSuffix, epoch) && epoch < keep_from_epoch;
            const bool tmp = include_tmp && StartsWith(name, kCheckpointPrefix) &&
                             EndsWith(name, std::string(kCheckpointSuffix) + kTmpSuffix);
            if (old_checkpoint || old_wal || tmp) {
                removed = fs_->Remove(JoinPath(dir_, name)) || removed;
            }
        }
        if (removed) {
            fs_->SyncDirectory(dir_);
        }
    } catch (const Error&) { // best effort: stale files are harmless and removed next time
    }
}

// ---------------------------------------------------------------------------------- commits

void StorageManager::CheckWritable() const {
    if (failed_.load()) {
        throw Error(ErrorCode::Io, "the database is read-only after an earlier I/O error on its "
                                   "write-ahead log; reopen it to recover");
    }
}

std::shared_ptr<Table> StorageManager::CreateTable(const std::string& name,
                                                   std::vector<ColumnDefinition> schema,
                                                   bool if_not_exists, idx_t row_group_size) {
    std::shared_ptr<Table> table;
    {
        const std::lock_guard<std::mutex> lock(commit_mutex_);
        CheckWritable();
        if (name.empty()) {
            throw Error(ErrorCode::Catalog, "table name must not be empty");
        }
        if (std::shared_ptr<Table> existing = catalog_.TryGetTable(name)) {
            if (if_not_exists) {
                return existing;
            }
            throw Error(ErrorCode::Catalog, "table \"" + name + "\" already exists");
        }
        table = std::make_shared<Table>(name, std::move(schema), row_group_size); // validates
        BinaryWriter w;
        w.U8(static_cast<uint8_t>(Op::CreateTable));
        WriteTableDefinition(w, *table);
        const uint64_t before = wal_->size();
        try {
            wal_->AppendFrame(w.buffer(), true);
            if (options_.sync == SyncMode::Full) {
                wal_->Sync();
            }
        } catch (...) {
            failed_ = true;
            throw;
        }
        transactions_total_++;
        bytes_uncheckpointed_ += wal_->size() - before;
        catalog_.AddTable(table);
    }
    MaybeCheckpoint();
    return table;
}

void StorageManager::DropTable(const std::string& name, bool if_exists) {
    {
        const std::lock_guard<std::mutex> lock(commit_mutex_);
        CheckWritable();
        if (catalog_.TryGetTable(name) == nullptr) {
            if (if_exists) {
                return;
            }
            throw Error(ErrorCode::Catalog, "table \"" + name + "\" does not exist");
        }
        BinaryWriter w;
        w.U8(static_cast<uint8_t>(Op::DropTable));
        w.String(name);
        const uint64_t before = wal_->size();
        try {
            wal_->AppendFrame(w.buffer(), true);
            if (options_.sync == SyncMode::Full) {
                wal_->Sync();
            }
        } catch (...) {
            failed_ = true;
            throw;
        }
        transactions_total_++;
        bytes_uncheckpointed_ += wal_->size() - before;
        catalog_.DropTable(name, true);
    }
    MaybeCheckpoint();
}

void StorageManager::LogAppend(const Table& target, const Table& staging) {
    std::vector<idx_t> columns;
    for (idx_t c = 0; c < staging.schema().size(); c++) {
        columns.push_back(c);
    }
    TableScan scan = staging.Scan(columns);
    DataChunk chunk;
    chunk.Initialize(scan.types(), kVectorSize);
    BinaryWriter w;
    while (scan.Next(chunk)) {
        w.U8(static_cast<uint8_t>(Op::Append));
        w.String(target.name());
        WriteChunk(w, chunk);
        if (w.size() >= options_.max_frame_bytes) {
            wal_->AppendFrame(w.buffer(), false);
            w.Clear();
        }
    }
    wal_->AppendFrame(w.buffer(), true); // the commit frame (possibly with nothing left to say)
}

void StorageManager::Append(const std::shared_ptr<Table>& target, std::unique_ptr<Table> staging) {
    if (staging->RowCount() == 0) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(commit_mutex_);
        CheckWritable();
        if (catalog_.TryGetTable(target->name()) != target) {
            throw Error(ErrorCode::Catalog,
                        "table \"" + target->name() +
                            "\" was dropped or replaced while the statement was "
                            "running");
        }
        const uint64_t before = wal_->size();
        try {
            LogAppend(*target, *staging);
            if (options_.sync == SyncMode::Full) {
                wal_->Sync();
            }
        } catch (...) {
            failed_ = true;
            throw;
        }
        transactions_total_++;
        bytes_uncheckpointed_ += wal_->size() - before;
        try {
            target->Merge(std::move(staging));
        } catch (...) {
            failed_ = true; // the log says the rows are there, memory says otherwise
            throw;
        }
    }
    MaybeCheckpoint();
}

// ---------------------------------------------------------------------------------- checkpoints

void StorageManager::MaybeCheckpoint() {
    if (options_.checkpoint_wal_bytes == 0 || failed_.load()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(commit_mutex_);
        if (bytes_uncheckpointed_ < options_.checkpoint_wal_bytes) {
            return;
        }
    }
    std::unique_lock<std::mutex> checkpoint(checkpoint_mutex_, std::try_to_lock);
    if (!checkpoint.owns_lock()) {
        return; // somebody else is already doing it
    }
    try {
        CheckpointLocked();
    } catch (const std::exception& e) {
        // the statement that got us here was committed; report the failure where it can be seen
        const std::lock_guard<std::mutex> lock(error_mutex_);
        last_checkpoint_error_ = e.what();
    }
}

void StorageManager::Checkpoint() {
    const std::lock_guard<std::mutex> lock(checkpoint_mutex_);
    CheckpointLocked();
}

void StorageManager::CheckpointLocked() {
    CheckpointImage image;
    uint64_t new_epoch = 0, transactions_at_rotation = 0, bytes_at_rotation = 0;
    {
        const std::lock_guard<std::mutex> lock(commit_mutex_);
        CheckWritable();
        if (transactions_total_ == transactions_checkpointed_) {
            return; // nothing was committed since the newest checkpoint
        }
        new_epoch = wal_epoch_.load() + 1;
        image.epoch = new_epoch;
        for (const std::string& name : catalog_.ListTables()) {
            const std::shared_ptr<Table> table = catalog_.GetTable(name);
            TableImage t;
            t.name = table->name();
            t.schema = table->schema();
            t.row_group_size = table->row_group_size();
            const auto snapshot = table->Snapshot();
            for (idx_t g = 0; g < snapshot->row_group_count(); g++) {
                t.groups.push_back(snapshot->row_group_ptr(g));
            }
            image.tables.push_back(std::move(t));
        }
        // From here on commits go to a new log, so the checkpoint can be written without holding
        // the commit mutex. The old log is complete and durable before the new one exists.
        try {
            wal_->Sync();
            auto fresh = WalWriter::Create(*fs_, JoinPath(dir_, WalName(new_epoch)), new_epoch);
            fs_->SyncDirectory(dir_);
            wal_ = std::move(fresh);
        } catch (...) {
            failed_ = true;
            throw;
        }
        wal_epoch_ = new_epoch;
        transactions_at_rotation = transactions_total_;
        bytes_at_rotation = bytes_uncheckpointed_;
    }

    const std::string final_path = JoinPath(dir_, CheckpointName(new_epoch));
    const std::string tmp_path = final_path + kTmpSuffix;
    const std::shared_ptr<TaskScheduler> pool = Scheduler();
    WriteCheckpoint(*fs_, tmp_path, image, pool.get());
    fs_->Rename(tmp_path, final_path);
    fs_->SyncDirectory(dir_);
    {
        const std::lock_guard<std::mutex> lock(commit_mutex_);
        checkpoint_epoch_ = new_epoch;
        transactions_checkpointed_ = transactions_at_rotation;
        bytes_uncheckpointed_ -= bytes_at_rotation;
    }
    RemoveObsoleteFiles(new_epoch, true);
}

} // namespace cdb
