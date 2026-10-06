#include "execution/task_scheduler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cdb {

namespace {
constexpr size_t kThreadCounts[] = {1, 2, 3, 4, 8};

// Spins (yielding) until `cond` holds or ten seconds pass; a hang becomes a test failure.
template <class F> bool WaitFor(F cond) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}
} // namespace

TEST(TaskScheduler, ThreadCountIsClampedAndZeroMeansOne) {
    EXPECT_EQ(TaskScheduler(0).threads(), 1U);
    EXPECT_EQ(TaskScheduler(1).threads(), 1U);
    EXPECT_EQ(TaskScheduler(5).threads(), 5U);
    EXPECT_GE(TaskScheduler::HardwareThreads(), 1U);
}

TEST(TaskScheduler, SingleThreadRunsTheBodyInlineOnTheCaller) {
    TaskScheduler scheduler(1);
    int calls = 0;
    std::thread::id where;
    scheduler.RunParallel(8, [&](size_t participant) {
        calls++;
        where = std::this_thread::get_id();
        EXPECT_EQ(participant, 0U);
    });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(where, std::this_thread::get_id());
}

TEST(TaskScheduler, ParallelForRunsEveryIndexExactlyOnce) {
    for (const size_t threads : kThreadCounts) {
        TaskScheduler scheduler(threads);
        for (const size_t n : {size_t{0}, size_t{1}, size_t{2}, size_t{7}, size_t{1000}}) {
            std::vector<std::atomic<int>> hits(n);
            scheduler.ParallelFor(n, [&](size_t i) { hits[i]++; });
            for (size_t i = 0; i < n; i++) {
                ASSERT_EQ(hits[i].load(), 1) << "threads " << threads << " n " << n << " i " << i;
            }
        }
    }
}

TEST(TaskScheduler, ParallelForHonoursTheParticipantLimit) {
    TaskScheduler scheduler(8);
    std::set<std::thread::id> ids;
    std::mutex mutex;
    scheduler.ParallelFor(
        200,
        [&](size_t) {
            const std::lock_guard<std::mutex> lock(mutex);
            ids.insert(std::this_thread::get_id());
        },
        /*max_participants=*/2);
    EXPECT_LE(ids.size(), 2U);
}

TEST(TaskScheduler, ParticipantsAreDistinctBoundedAndTheCallerIsZero) {
    for (const size_t threads : kThreadCounts) {
        TaskScheduler scheduler(threads);
        for (const size_t want : {size_t{1}, size_t{2}, size_t{4}, size_t{100}}) {
            std::mutex mutex;
            std::set<size_t> seen;
            scheduler.RunParallel(want, [&](size_t participant) {
                const std::lock_guard<std::mutex> lock(mutex);
                EXPECT_TRUE(seen.insert(participant).second) << "participant ids must be distinct";
            });
            const size_t limit = std::min(want, threads);
            EXPECT_GE(seen.size(), 1U);
            EXPECT_LE(seen.size(), limit);
            EXPECT_EQ(seen.count(0), 1U) << "the caller always participates";
            for (const size_t id : seen) {
                EXPECT_LT(id, limit);
            }
        }
    }
}

TEST(TaskScheduler, ParticipantZeroRunsOnTheCallingThread) {
    TaskScheduler scheduler(4);
    const std::thread::id caller = std::this_thread::get_id();
    for (int round = 0; round < 50; round++) {
        std::atomic<bool> zero_on_caller{false};
        scheduler.RunParallel(4, [&](size_t participant) {
            if (participant == 0) {
                zero_on_caller = std::this_thread::get_id() == caller;
            }
        });
        ASSERT_TRUE(zero_on_caller.load());
    }
}

TEST(TaskScheduler, ParticipantsReallyRunConcurrently) {
    // Four participants each wait for all four to have arrived: that only completes if they are
    // on four threads at once (no timing assumption beyond a generous timeout).
    TaskScheduler scheduler(4);
    std::atomic<size_t> arrived{0};
    std::atomic<size_t> saw_all{0};
    scheduler.RunParallel(4, [&](size_t) {
        arrived++;
        if (WaitFor([&] { return arrived.load() == 4; })) {
            saw_all++;
        }
    });
    EXPECT_EQ(arrived.load(), 4U);
    EXPECT_EQ(saw_all.load(), 4U);
}

TEST(TaskScheduler, ParticipantsShareWorkThroughACursor) {
    for (const size_t threads : kThreadCounts) {
        TaskScheduler scheduler(threads);
        constexpr uint64_t kItems = 100000;
        std::atomic<uint64_t> cursor{0};
        std::atomic<uint64_t> total{0};
        scheduler.RunParallel(threads, [&](size_t) {
            uint64_t local = 0;
            for (;;) {
                const uint64_t i = cursor.fetch_add(1);
                if (i >= kItems) {
                    break;
                }
                local += i;
            }
            total += local;
        });
        EXPECT_EQ(total.load(), kItems * (kItems - 1) / 2) << "threads " << threads;
    }
}

TEST(TaskScheduler, AnExceptionInTheCallerIsRethrownAfterEveryoneFinished) {
    TaskScheduler scheduler(4);
    std::atomic<int> finished{0};
    try {
        scheduler.RunParallel(4, [&](size_t participant) {
            if (participant == 0) {
                finished++;
                throw std::runtime_error("from the caller");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // still running
            finished++;
        });
        FAIL() << "expected an exception";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "from the caller");
    }
    // RunParallel must not return while a worker is still inside the body (it would use freed
    // captures): every participant that started has finished.
    EXPECT_GE(finished.load(), 1);
    const int after = finished.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(finished.load(), after) << "a worker was still running after RunParallel returned";
}

TEST(TaskScheduler, AnExceptionInAWorkerIsRethrownOnTheCaller) {
    TaskScheduler scheduler(4);
    std::atomic<size_t> arrived{0};
    EXPECT_THROW(scheduler.RunParallel(4,
                                       [&](size_t participant) {
                                           arrived++;
                                           WaitFor([&] { return arrived.load() == 4; });
                                           if (participant == 3) {
                                               throw std::logic_error("from a worker");
                                           }
                                       }),
                 std::logic_error);
}

TEST(TaskScheduler, ParallelForStopsHandingOutIndexesAfterAFailure) {
    TaskScheduler scheduler(4);
    std::atomic<size_t> ran{0};
    constexpr size_t kN = 1000000;
    EXPECT_THROW(scheduler.ParallelFor(kN,
                                       [&](size_t i) {
                                           ran++;
                                           if (i == 10) {
                                               throw std::runtime_error("boom");
                                           }
                                       }),
                 std::runtime_error);
    EXPECT_LT(ran.load(), kN) << "work kept being handed out after the failure";
}

TEST(TaskScheduler, TheSchedulerIsReusableAfterAFailure) {
    TaskScheduler scheduler(4);
    for (int round = 0; round < 20; round++) {
        EXPECT_THROW(scheduler.ParallelFor(100,
                                           [&](size_t i) {
                                               if (i == 50) {
                                                   throw std::runtime_error("boom");
                                               }
                                           }),
                     std::runtime_error);
        std::atomic<int> sum{0};
        scheduler.ParallelFor(100, [&](size_t i) { sum += static_cast<int>(i); });
        ASSERT_EQ(sum.load(), 4950);
    }
}

TEST(TaskScheduler, NestedJobsDoNotDeadlock) {
    for (const size_t threads : {size_t{2}, size_t{4}}) {
        TaskScheduler scheduler(threads);
        std::atomic<int> total{0};
        scheduler.ParallelFor(16, [&](size_t) {
            scheduler.ParallelFor(
                16, [&](size_t) { scheduler.ParallelFor(4, [&](size_t) { total++; }); });
        });
        EXPECT_EQ(total.load(), 16 * 16 * 4) << "threads " << threads;
    }
}

TEST(TaskScheduler, SeveralCallersShareOnePool) {
    TaskScheduler scheduler(4);
    constexpr int kCallers = 6;
    std::vector<std::thread> callers;
    std::atomic<int> failures{0};
    for (int c = 0; c < kCallers; c++) {
        callers.emplace_back([&, c] {
            for (int round = 0; round < 40; round++) {
                const size_t n = 50 + static_cast<size_t>(c) * 7 + static_cast<size_t>(round);
                std::atomic<uint64_t> sum{0};
                scheduler.ParallelFor(n, [&](size_t i) { sum += i; });
                if (sum.load() != n * (n - 1) / 2) {
                    failures++;
                }
            }
        });
    }
    for (std::thread& t : callers) {
        t.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

TEST(TaskScheduler, ManySmallJobsBackToBack) {
    TaskScheduler scheduler(8);
    std::atomic<uint64_t> total{0};
    for (int job = 0; job < 2000; job++) {
        scheduler.ParallelFor(3, [&](size_t i) { total += i + 1; });
    }
    EXPECT_EQ(total.load(), 2000U * 6U);
}

TEST(TaskScheduler, CreatingAndDestroyingPoolsIsClean) {
    for (int i = 0; i < 100; i++) {
        TaskScheduler scheduler(1 + static_cast<size_t>(i % 6));
        if (i % 3 == 0) {
            std::atomic<int> n{0};
            scheduler.ParallelFor(10, [&](size_t) { n++; });
            ASSERT_EQ(n.load(), 10);
        }
    }
}

} // namespace cdb
