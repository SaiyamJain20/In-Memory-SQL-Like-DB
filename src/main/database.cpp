#include "main/database.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace cdb {

size_t Database::DefaultThreads() {
    const char* env = std::getenv("CDB_THREADS");
    if (env == nullptr || *env == '\0') {
        return 1;
    }
    if (std::strcmp(env, "auto") == 0) {
        return TaskScheduler::HardwareThreads();
    }
    char* end = nullptr;
    const long n = std::strtol(env, &end, 10);
    if (end == env || *end != '\0' || n < 0) {
        return 1; // not a number: stay serial rather than guess
    }
    return n == 0 ? TaskScheduler::HardwareThreads() : static_cast<size_t>(n);
}

Database::Database() {
    SetThreads(DefaultThreads());
}

Database::Database(size_t threads) {
    SetThreads(threads);
}

size_t Database::threads() const {
    const std::lock_guard<std::mutex> lock(scheduler_mutex_);
    return threads_;
}

void Database::SetThreads(size_t threads) {
    if (threads == 0) {
        threads = TaskScheduler::HardwareThreads();
    }
    threads = std::min(threads, TaskScheduler::kMaxThreads);
    std::shared_ptr<TaskScheduler> pool =
        threads > 1 ? std::make_shared<TaskScheduler>(threads) : nullptr;
    const std::lock_guard<std::mutex> lock(scheduler_mutex_);
    threads_ = threads;
    scheduler_ = std::move(pool); // the old pool is destroyed once its last user lets go
}

std::shared_ptr<TaskScheduler> Database::scheduler() const {
    const std::lock_guard<std::mutex> lock(scheduler_mutex_);
    return scheduler_;
}

} // namespace cdb
