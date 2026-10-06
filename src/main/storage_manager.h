#pragma once

#include "catalog/catalog.h"
#include "execution/task_scheduler.h"
#include "io/file_system.h"
#include "storage/binary_io.h"
#include "storage/wal.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace cdb {

// How hard a commit tries to reach the disk.
enum class SyncMode : uint8_t {
    Full, // fsync the log before a statement returns: an acknowledged statement survives power loss
    Off,  // leave flushing to the OS: faster, a power cut may lose the last statements (never
          // reorders them or leaves half of one: recovery still yields a committed prefix)
};

struct StorageOptions {
    std::shared_ptr<FileSystem> fs; // null: the operating system's
    SyncMode sync = SyncMode::Full;
    // Take a checkpoint (and so empty the log) once this many bytes of log have accumulated; 0
    // never.
    uint64_t checkpoint_wal_bytes = 64ull << 20;
    // A statement's log frames are cut at about this size (a COPY of millions of rows is many).
    uint64_t max_frame_bytes = 16ull << 20;
    // Checkpoint when the database is closed if the log holds anything.
    bool checkpoint_on_close = true;
};

// What opening a database directory did.
struct RecoveryStats {
    bool created = false; // the directory held no database: a new one was started
    bool had_checkpoint = false;
    uint64_t checkpoint_epoch = 0;
    size_t tables = 0;         // loaded from the checkpoint
    size_t wal_files = 0;      // replayed
    uint64_t transactions = 0; // replayed
    uint64_t frames = 0;
    uint64_t discarded_bytes = 0; // the torn or uncommitted tail of the newest log
    uint64_t discarded_frames = 0;
};

// The durable half of a Database: owns the directory, its lock, the write-ahead log, checkpoints
// and recovery (ADR 0009). Every state-changing statement goes through here, which serialises them
// (one writer at a time; readers are never blocked) and makes each one a transaction:
//     validate / stage  ->  write the log record(s) and fsync  ->  apply to memory
// so an error before the log is written leaves nothing behind, and the in-memory apply cannot fail.
//
// Any failure while writing the log, or while switching to a new log, marks the manager *failed*:
// the database refuses further changes (reads still work) until it is reopened, because after a
// failed fsync or a half-written frame what the file holds is unknown. Recovery then decides.
class StorageManager {
  public:
    using SchedulerProvider = std::function<std::shared_ptr<TaskScheduler>()>;

    // Opens (creating if need be) the database in `dir`, loading it into `catalog`. Throws
    // Error(Io) if the directory cannot be used or is locked by another process, Error(Corruption)
    // if what is on disk does not verify (it is never "repaired" by guessing).
    StorageManager(Catalog& catalog, std::string dir, StorageOptions options,
                   SchedulerProvider scheduler);
    ~StorageManager();
    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    // ---- statements; each is atomic and durable (per SyncMode) when it returns ----------------
    // Same contract as Catalog::CreateTable / DropTable.
    std::shared_ptr<Table> CreateTable(const std::string& name,
                                       std::vector<ColumnDefinition> schema, bool if_not_exists,
                                       idx_t row_group_size);
    void DropTable(const std::string& name, bool if_exists);
    // Appends the rows of `staging` (a private table with `target`'s schema, consumed) to `target`.
    // Throws Error(Catalog) if `target` is no longer the table of that name.
    void Append(const std::shared_ptr<Table>& target, std::unique_ptr<Table> staging);

    // Writes a checkpoint now and starts a new log. A no-op if nothing was committed since the
    // last.
    void Checkpoint();

    const RecoveryStats& recovery() const noexcept { return recovery_; }
    uint64_t epoch() const noexcept { return wal_epoch_.load(); }
    uint64_t checkpoint_epoch() const noexcept { return checkpoint_epoch_.load(); }
    // Bytes of log that a checkpoint would fold away.
    uint64_t uncheckpointed_bytes() const;
    bool failed() const noexcept { return failed_.load(); }
    // The error of the last automatic checkpoint that failed (the commit that triggered it was
    // fine).
    std::string last_checkpoint_error() const;

    static std::string CheckpointName(uint64_t epoch);
    static std::string WalName(uint64_t epoch);

  private:
    void Recover();
    void LoadCheckpoint(const std::string& path);
    void ApplyPayload(const uint8_t* data, size_t size);
    void CheckWritable() const;
    // Commit machinery; the commit mutex is held.
    void CheckpointLocked();
    void LogAppend(const Table& target, const Table& staging);
    void MaybeCheckpoint();
    void RemoveObsoleteFiles(uint64_t keep_from_epoch, bool include_tmp);
    std::shared_ptr<TaskScheduler> Scheduler() const { return scheduler_ ? scheduler_() : nullptr; }

    Catalog& catalog_;
    std::string dir_;
    StorageOptions options_;
    SchedulerProvider scheduler_;
    std::shared_ptr<FileSystem> fs_;
    std::unique_ptr<FileLock> lock_;

    mutable std::mutex commit_mutex_; // serialises statements; guards wal_ and the counters below
    std::mutex checkpoint_mutex_;     // one checkpoint at a time
    std::unique_ptr<WalWriter> wal_;
    std::atomic<uint64_t> wal_epoch_{0};
    std::atomic<uint64_t> checkpoint_epoch_{0};
    std::atomic<bool> failed_{false};
    uint64_t transactions_total_ = 0;        // committed since the database was opened + replayed
    uint64_t transactions_checkpointed_ = 0; // how many of them the newest checkpoint holds
    uint64_t bytes_uncheckpointed_ = 0;
    mutable std::mutex error_mutex_;
    std::string last_checkpoint_error_;
    RecoveryStats recovery_;
};

} // namespace cdb
