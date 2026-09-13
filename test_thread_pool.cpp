#include "thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Multiplier {
    int multiply(int value) const {
        return value * 7;
    }
};

int increment(int value) {
    return value + 1;
}

struct MoveOnlyCallable {
    explicit MoveOnlyCallable(int offset) : offset_(std::make_unique<int>(offset)) {}

    MoveOnlyCallable(MoveOnlyCallable&&) noexcept = default;
    MoveOnlyCallable& operator=(MoveOnlyCallable&&) noexcept = default;
    MoveOnlyCallable(const MoveOnlyCallable&) = delete;
    MoveOnlyCallable& operator=(const MoveOnlyCallable&) = delete;

    int operator()(std::unique_ptr<int> value) {
        return *offset_ + *value;
    }

    std::unique_ptr<int> offset_;
};

struct LvalueRvalueCallable {
    int operator()() & {
        return 11;
    }

    long operator()() && {
        return 22;
    }
};

struct LockProbe {
    LockProbe(mylib::ThreadPool* poolPtr, std::atomic<bool>* destroyedPtr)
        : pool(poolPtr), destroyed(destroyedPtr) {}

    mylib::ThreadPool* pool;
    std::atomic<bool>* destroyed;

    ~LockProbe() {
        // If Discard destroys the task while holding the pool mutex, this call
        // deadlocks and the CTest timeout exposes the regression.
        (void)pool->pendingTasks();
        destroyed->store(true);
    }
};

void testSubmitReturnVoidAndExceptions() {
    mylib::ThreadPool pool(4);

    auto sum = pool.submit([](int a, int b) { return a + b; }, 1, 2);
    check(sum.get() == 3, "submit should return the callable result");

    auto voidResult = pool.submit([] {});
    voidResult.get();

    auto functionResult = pool.submit(increment, 5);
    check(functionResult.get() == 6, "submit should support ordinary functions");

    Multiplier multiplier;
    auto memberResult = pool.submit(&Multiplier::multiply, &multiplier, 6);
    check(memberResult.get() == 42, "submit should support member function pointers");

    try {
        auto exceptionResult = pool.submit(
            []() -> int { throw std::runtime_error("boom"); });
        (void)exceptionResult.get();
        check(false, "std::exception should propagate through future.get()");
    } catch (const std::runtime_error& error) {
        check(std::string(error.what()) == "boom",
              "std::exception payload should be preserved");
    }

    try {
        auto nonStdException = pool.submit([]() -> int { throw 42; });
        (void)nonStdException.get();
        check(false, "non-std exception should propagate through future.get()");
    } catch (int value) {
        check(value == 42, "non-std exception payload should be preserved");
    }

    pool.shutdown();
}

void testMoveOnlyCallableAndArguments() {
    mylib::ThreadPool pool(2);

    auto future = pool.submit(
        MoveOnlyCallable(10), std::make_unique<int>(5));
    check(future.get() == 15, "move-only callable and argument should work");

    auto lambda = [value = std::make_unique<int>(8)](
                      std::unique_ptr<int> input) mutable {
        return *value + *input;
    };
    auto lambdaFuture = pool.submit(std::move(lambda), std::make_unique<int>(4));
    check(lambdaFuture.get() == 12, "move-only lambda should work");

    pool.shutdown();
}

void testInvokeResultUsesLvalueCallable() {
    mylib::ThreadPool pool(1);

    auto submitFuture = pool.submit(LvalueRvalueCallable{});
    static_assert(std::is_same_v<decltype(submitFuture.get()), int>,
                  "submit must deduce InvokeResult<Func&, Args...>");
    check(submitFuture.get() == 11,
          "submit must invoke a stored callable as an lvalue");

    auto optionalFuture = pool.try_submit(LvalueRvalueCallable{});
    static_assert(std::is_same_v<decltype(optionalFuture->get()), int>,
                  "try_submit must deduce InvokeResult<Func&, Args...>");
    check(optionalFuture.has_value(), "try_submit should accept an open unbounded pool");
    check(optionalFuture->get() == 11,
          "try_submit must invoke a stored callable as an lvalue");

    pool.shutdown();
}

void testWaitWaitsWhileActiveWhenQueueIsEmpty() {
    mylib::ThreadPool pool(1);
    std::promise<void> started;
    std::promise<void> release;
    std::future<void> startedFuture = started.get_future();
    std::future<void> releaseFuture = release.get_future();

    auto activeFuture = pool.submit([&started, &releaseFuture] {
        started.set_value();
        releaseFuture.wait();
    });
    startedFuture.wait();

    check(pool.pendingTasks() == 0, "active task should have left the queue");
    check(pool.activeTasks() == 1, "active task should be observable");

    std::atomic<bool> waitReturned{false};
    std::promise<void> waiterStarted;
    std::future<void> waiterStartedFuture = waiterStarted.get_future();
    std::thread waiter([&pool, &waitReturned, &waiterStarted] {
        waiterStarted.set_value();
        pool.wait();
        waitReturned.store(true);
    });
    waiterStartedFuture.wait();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!waitReturned.load(), "wait must block while activeTasks() > 0");

    release.set_value();
    activeFuture.get();
    waiter.join();
    check(waitReturned.load(), "wait should return after the active task finishes");

    pool.shutdown();
}

void testBoundedSubmitDiscardAndBrokenPromise() {
    mylib::ThreadPool pool(1, 1);
    std::promise<void> started;
    std::promise<void> release;
    std::future<void> startedFuture = started.get_future();
    std::future<void> releaseFuture = release.get_future();

    auto activeFuture = pool.submit([&started, &releaseFuture] {
        started.set_value();
        releaseFuture.wait();
        return 7;
    });
    startedFuture.wait();

    std::atomic<bool> probeDestroyed{false};
    auto probe = std::make_shared<LockProbe>(&pool, &probeDestroyed);
    auto queuedFuture = pool.submit([probe] { return 8; });
    probe.reset();

    check(pool.pendingTasks() == 1, "bounded queue should contain one task");
    check(pool.activeTasks() == 1, "bounded pool should execute one task");

    std::atomic<bool> blockedSubmitReturned{false};
    std::atomic<int> blockedSubmitOutcome{0};
    std::promise<void> blockedSubmitStarted;
    std::future<void> blockedSubmitStartedFuture =
        blockedSubmitStarted.get_future();

    std::thread blockedSubmitter(
        [&pool, &blockedSubmitReturned, &blockedSubmitOutcome,
         &blockedSubmitStarted] {
            blockedSubmitStarted.set_value();
            try {
                (void)pool.submit([] { return 9; });
                blockedSubmitOutcome.store(2);
            } catch (const std::runtime_error&) {
                blockedSubmitOutcome.store(1);
            } catch (...) {
                blockedSubmitOutcome.store(3);
            }
            blockedSubmitReturned.store(true);
        });
    blockedSubmitStartedFuture.wait();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!blockedSubmitReturned.load(),
          "submit should block when the bounded queue is full");

    std::thread stopper([&pool] {
        pool.shutdown(mylib::ShutdownMode::Discard);
    });
    while (!pool.isShutdown()) {
        std::this_thread::yield();
    }
    release.set_value();
    stopper.join();
    blockedSubmitter.join();

    check(blockedSubmitOutcome.load() == 1,
          "shutdown must wake a blocked submit with runtime_error");
    check(blockedSubmitReturned.load(),
          "blocked submitter should return after shutdown");
    check(activeFuture.get() == 7, "active task should finish before shutdown returns");
    check(probeDestroyed.load(),
          "discarded task must be destroyed outside the pool mutex");

    try {
        (void)queuedFuture.get();
        check(false, "discarded task future should throw broken_promise");
    } catch (const std::future_error& error) {
        check(error.code() == std::future_errc::broken_promise,
              "discarded task future should report broken_promise");
    }

    check(pool.isShutdown(), "isShutdown should be true after shutdown");
    check(pool.pendingTasks() == 0, "Discard should clear pending tasks");
    check(pool.activeTasks() == 0, "shutdown should wait for active tasks");
}

void testTrySubmitFailureDoesNotConsumeArguments() {
    mylib::ThreadPool pool(1, 1);
    std::promise<void> started;
    std::promise<void> release;
    std::future<void> startedFuture = started.get_future();
    std::future<void> releaseFuture = release.get_future();

    auto activeFuture = pool.submit([&started, &releaseFuture] {
        started.set_value();
        releaseFuture.wait();
    });
    startedFuture.wait();
    auto queuedFuture = pool.submit([] { return 1; });

    std::unique_ptr<int> fullArgument = std::make_unique<int>(42);
    auto fullResult = pool.try_submit(
        [](std::unique_ptr<int> value) { return *value; },
        std::move(fullArgument));
    check(!fullResult.has_value(), "try_submit should return nullopt for a full queue");
    check(fullArgument != nullptr && *fullArgument == 42,
          "failed try_submit must not consume a full-queue argument");

    release.set_value();
    activeFuture.get();
    check(queuedFuture.get() == 1, "queued task should run before Drain shutdown");
    pool.shutdown(mylib::ShutdownMode::Drain);

    std::unique_ptr<int> closedArgument = std::make_unique<int>(99);
    auto closedResult = pool.try_submit(
        [](std::unique_ptr<int> value) { return *value; },
        std::move(closedArgument));
    check(!closedResult.has_value(), "try_submit should return nullopt when closed");
    check(closedArgument != nullptr && *closedArgument == 99,
          "failed try_submit must not consume a closed-pool argument");
}

void testShutdownFirstModeWinsAndBoolCompatibility() {
    mylib::ThreadPool drainPool(1, 1);
    std::promise<void> started;
    std::promise<void> release;
    std::future<void> startedFuture = started.get_future();
    std::future<void> releaseFuture = release.get_future();

    auto activeFuture = drainPool.submit([&started, &releaseFuture] {
        started.set_value();
        releaseFuture.wait();
    });
    startedFuture.wait();
    auto queuedFuture = drainPool.submit([] { return 55; });

    std::promise<void> shutdownStarted;
    std::future<void> shutdownStartedFuture = shutdownStarted.get_future();
    std::thread drainer([&drainPool, &shutdownStarted] {
        shutdownStarted.set_value();
        drainPool.shutdown(mylib::ShutdownMode::Drain);
    });
    shutdownStartedFuture.wait();
    while (!drainPool.isShutdown()) {
        std::this_thread::yield();
    }
    release.set_value();
    drainer.join();
    activeFuture.get();
    check(queuedFuture.get() == 55,
          "first shutdown mode Drain must be retained by later calls");

    drainPool.shutdown(mylib::ShutdownMode::Discard);
    drainPool.shutdown(false);
    check(drainPool.isShutdown(), "repeated shutdown must remain idempotent");

    mylib::ThreadPool boolPool(1);
    auto boolFuture = boolPool.submit([] { return 123; });
    boolPool.shutdown(true);
    check(boolFuture.get() == 123,
          "shutdown(true) should retain Drain compatibility semantics");
}

void testWaitWaitsOnlyForTasksSubmittedBeforeItStarted() {
    mylib::ThreadPool pool(2);
    std::promise<void> slowStarted;
    std::promise<void> releaseSlow;
    std::future<void> slowStartedFuture = slowStarted.get_future();
    std::future<void> releaseSlowFuture = releaseSlow.get_future();

    auto slowFuture = pool.submit([&slowStarted, &releaseSlowFuture] {
        slowStarted.set_value();
        releaseSlowFuture.wait();
    });
    slowStartedFuture.wait();

    std::atomic<bool> waiterStarted{false};
    std::atomic<bool> waitReturned{false};
    std::thread waiter([&pool, &waiterStarted, &waitReturned] {
        waiterStarted.store(true, std::memory_order_release);
        pool.wait();
        waitReturned.store(true, std::memory_order_release);
    });
    while (!waiterStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto laterFuture = pool.submit([] {});
    laterFuture.get();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    check(!waitReturned.load(std::memory_order_acquire),
          "wait must not be satisfied by tasks submitted after it started");

    releaseSlow.set_value();
    slowFuture.get();
    waiter.join();
    check(waitReturned.load(std::memory_order_acquire),
          "wait should return after all tasks submitted before it started finish");

    pool.shutdown();
}

void testDestructorDrainsByDefault() {
    std::future<int> future;
    {
        mylib::ThreadPool pool(2, 0);
        future = pool.submit([] { return 77; });
    }
    check(future.get() == 77, "destructor should drain submitted tasks");
}

void testZeroThreadsAndObservability() {
    mylib::ThreadPool pool(0);
    check(pool.threadCount() == 1, "zero requested threads should become one thread");
    check(!pool.isShutdown(), "a new pool should be open");
    check(pool.pendingTasks() == 0, "a new pool should have no pending tasks");
    check(pool.activeTasks() == 0, "a new pool should have no active tasks");

    auto future = pool.submit([] { return 1; });
    check(future.get() == 1, "zero-thread fallback pool should execute tasks");

    pool.shutdown();
    check(pool.isShutdown(), "shutdown should be observable");
    check(pool.pendingTasks() == 0, "shutdown should leave no pending tasks");
    check(pool.activeTasks() == 0, "shutdown should leave no active tasks");
    check(pool.threadCount() == 1, "threadCount should report joined workers");
}

void testConcurrentShutdownAndSubmit() {
    mylib::ThreadPool pool(4);
    constexpr int submitterCount = 8;
    constexpr int shutdownCount = 8;
    constexpr int submissionsPerThread = 200;

    std::atomic<bool> start{false};
    std::atomic<bool> unexpectedError{false};
    std::atomic<unsigned int> executed{0};

    std::thread submitters[submitterCount];
    for (int i = 0; i < submitterCount; ++i) {
        submitters[i] = std::thread([&pool, &start, &unexpectedError, &executed] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int j = 0; j < submissionsPerThread; ++j) {
                try {
                    (void)pool.submit([&executed] {
                        executed.fetch_add(1, std::memory_order_relaxed);
                    });
                } catch (const std::runtime_error&) {
                    break;
                } catch (...) {
                    unexpectedError.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }

    std::thread shutdowners[shutdownCount];
    for (int i = 0; i < shutdownCount; ++i) {
        shutdowners[i] = std::thread([&pool, &start, i] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            pool.shutdown(i % 2 == 0 ? mylib::ShutdownMode::Drain
                                     : mylib::ShutdownMode::Discard);
        });
    }

    start.store(true, std::memory_order_release);

    for (std::thread& submitter : submitters) {
        submitter.join();
    }
    for (std::thread& shutdowner : shutdowners) {
        shutdowner.join();
    }

    check(!unexpectedError.load(),
          "concurrent submit should fail only with runtime_error after shutdown");
    check(pool.isShutdown(), "concurrent shutdown should leave the pool closed");
    check(pool.pendingTasks() == 0, "concurrent shutdown should clear pending state");
    check(pool.activeTasks() == 0, "concurrent shutdown should join all workers");
}

}  // namespace

int main() {
    testSubmitReturnVoidAndExceptions();
    testMoveOnlyCallableAndArguments();
    testInvokeResultUsesLvalueCallable();
    testWaitWaitsWhileActiveWhenQueueIsEmpty();
    testBoundedSubmitDiscardAndBrokenPromise();
    testTrySubmitFailureDoesNotConsumeArguments();
    testShutdownFirstModeWinsAndBoolCompatibility();
    testWaitWaitsOnlyForTasksSubmittedBeforeItStarted();
    testDestructorDrainsByDefault();
    testZeroThreadsAndObservability();
    testConcurrentShutdownAndSubmit();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All thread pool tests passed\n";
    return 0;
}
