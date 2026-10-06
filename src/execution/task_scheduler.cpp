#include "execution/task_scheduler.h"

#include <algorithm>
#include <atomic>

namespace cdb {

// A job as the pool sees it. Every field is guarded by TaskScheduler::mutex_.
struct TaskScheduler::Job {
    Job(const std::function<void(size_t)>* b, size_t max) : body(b), max_participants(max) {}

    const std::function<void(size_t)>* body;
    size_t max_participants;
    size_t joined = 1;  // participant ids handed out so far (the caller is 0)
    size_t running = 0; // workers currently inside body
    std::condition_variable done;
    std::exception_ptr error; // first exception thrown by a worker
};

size_t TaskScheduler::HardwareThreads() noexcept {
    return std::max<size_t>(1, std::thread::hardware_concurrency());
}

TaskScheduler::TaskScheduler(size_t threads)
    : threads_(std::clamp<size_t>(threads, 1, kMaxThreads)) {
    try {
        for (size_t i = 1; i < threads_; i++) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    } catch (...) { // could not start a thread: stop the ones that did start, then fail
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (std::thread& w : workers_) {
            w.join();
        }
        throw;
    }
}

TaskScheduler::~TaskScheduler() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& w : workers_) {
        w.join();
    }
}

void TaskScheduler::WorkerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (stopping_) {
            return;
        }
        Job* job = jobs_.front();
        const size_t id = job->joined++;
        if (job->joined >= job->max_participants) {
            jobs_.pop_front(); // fully staffed
        }
        job->running++;
        lock.unlock();
        std::exception_ptr error;
        try {
            (*job->body)(id);
        } catch (...) {
            error = std::current_exception();
        }
        lock.lock();
        if (error && !job->error) {
            job->error = error;
        }
        if (--job->running == 0) {
            job->done.notify_all(); // the caller may destroy the job once it gets the lock back
        }
    }
}

void TaskScheduler::RunParallel(size_t max_participants, const std::function<void(size_t)>& body) {
    const size_t participants = std::clamp<size_t>(max_participants, 1, threads_);
    if (participants == 1) {
        body(0);
        return;
    }
    Job job(&body, participants);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(&job);
    }
    for (size_t i = 1; i < participants; i++) {
        wake_.notify_one();
    }
    std::exception_ptr error;
    try {
        body(0);
    } catch (...) {
        error = std::current_exception();
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Close the job to newcomers, then wait for the workers already inside it.
        const auto it = std::find(jobs_.begin(), jobs_.end(), &job);
        if (it != jobs_.end()) {
            jobs_.erase(it);
        }
        job.done.wait(lock, [&job] { return job.running == 0; });
        if (!error) {
            error = job.error;
        }
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

void TaskScheduler::ParallelFor(size_t n, const std::function<void(size_t)>& fn,
                                size_t max_participants) {
    if (n == 0) {
        return;
    }
    const size_t participants = std::min(n, max_participants == 0 ? threads_ : max_participants);
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    RunParallel(participants, [&](size_t) {
        while (!failed.load(std::memory_order_relaxed)) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) {
                return;
            }
            try {
                fn(i);
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
                throw;
            }
        }
    });
}

} // namespace cdb
