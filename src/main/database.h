#pragma once

#include "catalog/catalog.h"
#include "execution/task_scheduler.h"

#include <memory>
#include <mutex>

namespace cdb {

// An in-process database instance. Owns the catalog and the thread pool queries run on.
class Database {
  public:
    // Starts with DefaultThreads() threads.
    Database();
    explicit Database(size_t threads);

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

  private:
    Catalog catalog_;
    mutable std::mutex scheduler_mutex_;
    size_t threads_ = 1;
    std::shared_ptr<TaskScheduler> scheduler_;
};

} // namespace cdb
