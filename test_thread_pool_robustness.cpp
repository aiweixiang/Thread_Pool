#include "thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<int> failures{0};

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        failures.fetch_add(1, std::memory_order_relaxed);
    }
}

struct ThrowingCopy {
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy&) {
        throw std::runtime_error("copy constructor");
    }
};

struct ThrowingMove {
    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove&) {}
    ThrowingMove(ThrowingMove&&) {
        throw std::runtime_error("move constructor");
    }
};

struct ThrowingAlloc {
    ThrowingAlloc() = default;
    ThrowingAlloc(const ThrowingAlloc&) {
        throw std::bad_alloc();
    }
    ThrowingAlloc(ThrowingAlloc&&) {
        throw std::bad_alloc();
    }
};

void testTaskConstructionExceptions() {
    {
        mylib::ThreadPool pool(2);
        bool threw = false;
        try {
            (void)pool.submit([](ThrowingCopy) { return 1; }, ThrowingCopy{});
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "submit should propagate a parameter copy exception");
        check(pool.pendingTasks() == 0 && pool.activeTasks() == 0,
              "failed parameter copy must not enqueue a task");
        pool.wait();
        check(pool.submit([] { return 7; }).get() == 7,
              "pool should remain usable after a parameter copy failure");
    }

    {
        mylib::ThreadPool pool(2);
        bool threw = false;
        try {
            (void)pool.submit([](ThrowingMove) { return 1; }, ThrowingMove{});
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "submit should propagate a parameter move exception");
        pool.wait();
        check(pool.submit([] { return 8; }).get() == 8,
              "pool should remain usable after a parameter move failure");
    }

    {
        mylib::ThreadPool pool(2);
        bool threw = false;
        try {
            (void)pool.submit([](ThrowingAlloc) { return 1; }, ThrowingAlloc{});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        check(threw, "submit should propagate an allocation exception from a parameter");
        pool.wait();
        check(pool.submit([] { return 9; }).get() == 9,
              "pool should remain usable after a parameter allocation failure");
    }

    {
        mylib::ThreadPool pool(1, 4);
        bool threw = false;
        try {
            (void)pool.try_submit(
                [](ThrowingCopy) { return 1; }, ThrowingCopy{});
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "try_submit should propagate a user construction exception");
        check(pool.pendingTasks() == 0,
              "failed try_submit construction must not consume queue capacity");

        std::vector<std::future<int>> futures;
        for (int i = 0; i < 4; ++i) {
            auto optional = pool.try_submit([i] { return 10 + i; });
            check(optional.has_value(),
                  "queue capacity should remain available after try_submit failure");
            if (optional) {
                futures.push_back(std::move(*optional));
            }
        }
        for (std::size_t i = 0; i < futures.size(); ++i) {
            check(futures[i].get() == 10 + static_cast<int>(i),
                  "task submitted after try_submit failure should complete");
        }
    }

    {
        mylib::ThreadPool pool(3);
        for (int i = 0; i < 200; ++i) {
            try {
                (void)pool.submit(
                    [](ThrowingCopy) { return 1; }, ThrowingCopy{});
            } catch (const std::runtime_error&) {
            }
        }
        pool.wait();
        check(pool.submit([] { return 42; }).get() == 42,
              "repeated construction failures must not poison completion tracking");
    }
}

struct Accounting {
    std::atomic<unsigned int> executed{0};
    std::atomic<unsigned int> accepted{0};
    std::atomic<unsigned int> rejectedClosed{0};
    std::atomic<unsigned int> rejectedFull{0};
    std::atomic<unsigned int> unexpected{0};
    std::mutex futuresMutex;
    std::vector<std::future<void>> futures;

    void keep(std::future<void> future) {
        std::lock_guard<std::mutex> lock(futuresMutex);
        futures.push_back(std::move(future));
    }
};

struct AccountingResult {
    unsigned int fulfilled = 0;
    unsigned int broken = 0;
    unsigned int notReady = 0;
    unsigned int bad = 0;
};

void verifyAccounting(const char* name, const Accounting& accounting,
                      AccountingResult result) {
    const unsigned int accepted = accounting.accepted.load();
    const unsigned int executed = accounting.executed.load();

    std::cout << name << ": accepted=" << accepted
              << " executed=" << executed
              << " fulfilled=" << result.fulfilled
              << " broken=" << result.broken
              << " notReady=" << result.notReady << '\n';

    check(accepted >= 2, "accounting test should accept a meaningful number of tasks");
    check(accounting.unexpected.load() == 0, "submitters should not see unexpected errors");
    check(result.notReady == 0, "every accepted future should become ready");
    check(result.bad == 0, "future failures should only be broken_promise");
    check(result.fulfilled + result.broken == accepted,
          "fulfilled + broken should equal accepted");
    check(result.fulfilled == executed, "fulfilled should equal executed");
}

void runAccountingCase(const char* name, std::size_t workers,
                       std::size_t queueCapacity, mylib::ShutdownMode mode,
                       unsigned int targetAccepted) {
    mylib::ThreadPool pool(workers, queueCapacity);
    Accounting accounting;
    std::atomic<bool> start{false};
    std::atomic<bool> stopProducing{false};
    std::atomic<bool> releaseWork{false};
    std::atomic<bool> stopWaiters{false};
    std::vector<std::thread> threads;

    auto work = [&releaseWork, &accounting] {
        while (!releaseWork.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        accounting.executed.fetch_add(1, std::memory_order_relaxed);
    };

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int j = 0; j < 4000 && !stopProducing.load(std::memory_order_relaxed);
                 ++j) {
                try {
                    accounting.keep(pool.submit(work));
                    accounting.accepted.fetch_add(1, std::memory_order_relaxed);
                } catch (const std::runtime_error&) {
                    accounting.rejectedClosed.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    accounting.unexpected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int j = 0; j < 4000 && !stopProducing.load(std::memory_order_relaxed);
                 ++j) {
                try {
                    auto optional = pool.try_submit(work);
                    if (optional) {
                        accounting.keep(std::move(*optional));
                        accounting.accepted.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        accounting.rejectedFull.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    accounting.unexpected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stopWaiters.load(std::memory_order_relaxed)) {
                pool.wait();
            }
        });
    }

    start.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (accounting.accepted.load(std::memory_order_relaxed) < targetAccepted ||
           pool.pendingTasks() == 0 || pool.activeTasks() != workers) {
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::yield();
    }

    check(accounting.accepted.load() >= targetAccepted,
          "producer threshold should be reached before shutdown");
    check(pool.pendingTasks() > 0,
          "accounting test should have queued work when shutdown starts");
    check(pool.activeTasks() == workers,
          "all workers should be occupied when shutdown starts");

    stopProducing.store(true, std::memory_order_relaxed);
    std::thread shutdowner([&pool, mode] {
        pool.shutdown(mode);
    });

    while (!pool.isShutdown()) {
        std::this_thread::yield();
    }
    releaseWork.store(true, std::memory_order_release);

    shutdowner.join();
    stopWaiters.store(true, std::memory_order_relaxed);
    for (std::thread& thread : threads) {
        thread.join();
    }

    AccountingResult result;
    for (std::future<void>& future : accounting.futures) {
        if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            ++result.notReady;
            continue;
        }
        try {
            future.get();
            ++result.fulfilled;
        } catch (const std::future_error& error) {
            if (error.code() == std::future_errc::broken_promise) {
                ++result.broken;
            } else {
                ++result.bad;
            }
        } catch (...) {
            ++result.bad;
        }
    }

    verifyAccounting(name, accounting, result);
    if (mode == mylib::ShutdownMode::Drain) {
        check(result.broken == 0, "Drain should not discard accepted tasks");
    } else {
        check(result.broken > 0, "Discard should leave deterministic queued work broken");
    }
    check(pool.pendingTasks() == 0 && pool.activeTasks() == 0,
          "shutdown should leave no pending or active tasks");
}

void testConcurrentAccountingAndWaitCoverage() {
    runAccountingCase("bounded(2, 4) + Drain", 2, 4,
                      mylib::ShutdownMode::Drain, 6);
    runAccountingCase("bounded(2, 4) + Discard", 2, 4,
                      mylib::ShutdownMode::Discard, 6);
    runAccountingCase("bounded(1, 1) + Discard", 1, 1,
                      mylib::ShutdownMode::Discard, 2);
    runAccountingCase("unbounded(4) + Discard", 4, 0,
                      mylib::ShutdownMode::Discard, 64);
}

}  // namespace

int main() {
    testTaskConstructionExceptions();
    testConcurrentAccountingAndWaitCoverage();

    const int failureCount = failures.load(std::memory_order_relaxed);
    if (failureCount != 0) {
        std::cerr << failureCount << " robustness test(s) failed\n";
        return 1;
    }

    std::cout << "All thread pool robustness tests passed\n";
    return 0;
}
