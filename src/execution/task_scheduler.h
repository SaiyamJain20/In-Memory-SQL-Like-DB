#pragma once

#include "common/types.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cdb {

// A fixed pool of worker threads that cooperate on "parallel jobs" (morsel-driven parallelism).
//
// A job is one function, body(participant), that every participating thread runs once; the
// participants share the work through state the body captures (an atomic cursor over morsels, a
// counter of tasks, ...) and return when none is left. That is the shape of a pipeline: each
// participant owns its local operator and sink state and pulls morsels until the source is
// exhausted. There is no per-task queue: a thread that finishes early simply returns.
//
// The calling thread is always participant 0 and runs the body itself, so a job makes progress even
// when every worker is busy with someone else's job (several connections sharing one pool) and
// nested jobs cannot deadlock. Workers join a job while it has room and it is still running; late
// arrivals find nothing to do and leave.
class TaskScheduler {
  public:
    // `threads` is the total parallelism including the calling thread (so threads - 1 workers are
    // started). 0 is treated as 1. Capped at kMaxThreads.
    explicit TaskScheduler(size_t threads);
    ~TaskScheduler();
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    static constexpr size_t kMaxThreads = 256;

    size_t threads() const noexcept { return threads_; }

    // Runs body(participant) on up to `max_participants` threads (at least the caller) and returns
    // when every participant has returned. Participant ids are distinct and < max_participants; the
    // caller is 0. If any body throws, the first exception is rethrown here after all participants
    // have returned. Participants that should stop early when another one fails have to share a
    // flag of their own (ParallelFor does).
    void RunParallel(size_t max_participants, const std::function<void(size_t participant)>& body);

    // Runs fn(i) for every i in [0, n), dynamically distributed over up to `max_participants`
    // threads (default: all). Stops handing out indexes once an exception was thrown.
    void ParallelFor(size_t n, const std::function<void(size_t i)>& fn,
                     size_t max_participants = 0);

    // hardware_concurrency() with a sane floor of 1.
    static size_t HardwareThreads() noexcept;

  private:
    struct Job;
    void WorkerLoop();

    const size_t threads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job*> jobs_; // jobs that still have room for another participant
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

} // namespace cdb
