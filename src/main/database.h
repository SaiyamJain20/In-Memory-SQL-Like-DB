#pragma once

#include "catalog/catalog.h"
#include "execution/task_scheduler.h"
#include "main/storage_manager.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace cdb {

struct DatabaseOptions {
    // Threads a query may use; unset: Database::DefaultThreads().
    std::optional<size_t> threads;
    // Persistent databases only (see StorageOptions).
    StorageOptions storage;
    // Rows per row group of tables created by SQL (tests use small ones to cross many boundaries).
    idx_t row_group_size = kRowGroupSize;
};

// A database instance. Owns the catalog and the thread pool queries run on; given a path, also a
// directory on disk that makes every committed statement durable and is recovered when the
// database is opened again (ADR 0009).
class Database {
  public:
    // In memory, with DefaultThreads() threads.
    Database();
    // In memory.
    explicit Database(size_t threads);
    // Opens (creating if need be) the database stored in directory `path`. Throws Error(Io) if the
    // directory is unusable or in use by another process, Error(Corruption) if its files do not
    // verify.
    explicit Database(const std::string& path, DatabaseOptions options = {});
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Catalog& catalog() noexcept { return catalog_; }
    const Catalog& catalog() const noexcept { return catalog_; }

    // The number of threads a query may use, including the thread that runs it. 1 means no
    // parallelism: queries run on the calling thread.
    size_t threads() const;
    // Changes the thread count (0 means one thread per hardware thread). Queries already running
    // keep the pool they started with.
    void SetThreads(size_t threads);

    // The pool queries run on, or null when threads() == 1. Shared so a running query keeps it
    // alive across a SetThreads().
    std::shared_ptr<TaskScheduler> scheduler() const;

    // 1, unless the environment variable CDB_THREADS says otherwise (a number, or 0 / "auto" for
    // one thread per hardware thread). Library users opt in to parallelism; the shell and the
    // benchmarks default to all hardware threads.
    static size_t DefaultThreads();

    // ---- the state-changing statements, in memory or durably --------------------------------
    // Same contract as Catalog::CreateTable / DropTable, but logged when the database is
    // persistent.
    std::shared_ptr<Table> CreateTable(const std::string& name,
                                       std::vector<ColumnDefinition> schema, bool if_not_exists);
    void DropTable(const std::string& name, bool if_exists);
    // Appends `staging` (a private table with `target`'s schema, consumed) to `target`, atomically.
    // Throws Error(Catalog) if `target` is no longer the table of that name.
    void CommitAppend(const std::shared_ptr<Table>& target, std::unique_ptr<Table> staging);
    // Writes a checkpoint (folds the log into a new snapshot). A no-op in memory.
    void Checkpoint();

    bool persistent() const noexcept { return storage_ != nullptr; }
    // Null for an in-memory database.
    StorageManager* storage() noexcept { return storage_.get(); }
    idx_t row_group_size() const noexcept { return row_group_size_; }

  private:
    Catalog catalog_;
    idx_t row_group_size_ = kRowGroupSize;
    mutable std::mutex scheduler_mutex_;
    size_t threads_ = 1;
    std::shared_ptr<TaskScheduler> scheduler_;
    // Declared last: closed first, while the catalog and the pool it uses are still alive.
    std::unique_ptr<StorageManager> storage_;
};

} // namespace cdb
