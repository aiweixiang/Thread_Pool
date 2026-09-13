#ifndef MYLIB_THREAD_POOL_HPP
#define MYLIB_THREAD_POOL_HPP

/*
 * 能力
 * - C++17 header-only 线程池，支持普通函数、lambda、函数对象和成员函数指针。
 * - submit() 返回 std::future<R>；try_submit() 返回 std::optional<std::future<R>>。
 * - 支持 move-only 可调用对象、move-only 参数、void/非 void 返回值和异常透传。
 * - maxQueueSize == 0 表示无界队列；maxQueueSize > 0 表示有界队列。
 *
 * 使用禁忌
 * - 任务内不递归 submit() 回同一线程池；需要时使用 try_submit()。
 * - 任务内不调用本线程池的 shutdown()、wait() 或析构函数。
 * - 任务不得同步等待同一线程池中尚未完成的其他任务；任务依赖应在池外调度。
 * - submit()/try_submit() 会在互斥锁保护下构造任务；可调用对象和入参的
 *   拷贝/移动构造函数不得回调本线程池的任何成员函数。
 * - 销毁线程池前必须 join 所有可能调用 submit()/try_submit()/wait()/
 *   shutdown() 的外部线程；shutdown() 返回不表示其他线程的调用已经返回。
 * - std::ref 不延长被引用对象的生命周期；该对象必须存活到任务执行结束。
 *
 * 已知语义
 * - try_submit() 只有在确认队列未满且线程池未关闭后才构造任务，因此失败返回
 *   std::nullopt 时不会消费入参。
 * - try_submit() 返回 std::nullopt 可能表示队列已满或线程池已关闭；可调用
 *   isShutdown() 区分，但并发状态下仍需处理关闭竞态。
 * - submit() 在线程池已关闭时抛 std::runtime_error；用户参数或可调用对象的
 *   构造也可能抛出 std::runtime_error。捕获后可用 isShutdown() 辅助判断，
 *   但不能仅凭异常类型区分所有情况。
 * - shutdown(Discard) 后，被丢弃任务的 future 会以 std::future_error
 *   (broken_promise) 结束。
 * - 并发 shutdown() 参数不同时，只有第一个把 stop_ 从 false 切换为 true
 *   的调用者的模式生效；所有调用者都在 worker join 完成后才返回。
 * - isShutdown() 返回 true 只表示关闭已经开始，不表示正在执行的任务已结束、
 *   worker 已 join 或资源已完全释放。
 * - wait() 只等待调用时刻之前已经成功提交的任务清空且不再执行，不保证之后
 *   不再提交任务；与 shutdown() 并发时，Discard 丢弃的任务也会被计入完成。
 * - 放弃或只等待 future 而不调用 get() 会失去任务异常；wait() 只报告就绪。
 * - threadCount() 返回构造时创建的 worker 槽位数（threadCount == 0 时按 1）；
 *   shutdown join 之后数值不变，不表示仍有活线程。
 */

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace mylib {

enum class ShutdownMode {
    Drain,
    Discard
};

class ThreadPool {
private:
    template <typename Func, typename... Args>
    using InvokeResult =
        std::invoke_result_t<std::decay_t<Func>&, std::decay_t<Args>...>;

    struct TaskBase {
        virtual ~TaskBase() = default;
        virtual void run() = 0;

        std::uint64_t id = 0;
    };

    template <typename Func, typename... Args>
    class PackagedTask final : public TaskBase {
    public:
        using Result = InvokeResult<Func, Args...>;

        template <typename F, typename... A>
        explicit PackagedTask(F&& func, A&&... args)
            : func_(std::forward<F>(func)),
              args_(std::forward<A>(args)...),
              task_([this]() -> Result { return invokeTask(); }) {}

        // task_ captures this. Copying or moving the object would leave the
        // packaged_task's shared state bound to the old address. Before making
        // this type movable, replace task_ with a std::promise<Result> member;
        // do not try to repair a move constructor around packaged_task.
        PackagedTask(const PackagedTask&) = delete;
        PackagedTask& operator=(const PackagedTask&) = delete;
        PackagedTask(PackagedTask&&) = delete;
        PackagedTask& operator=(PackagedTask&&) = delete;

        void run() override {
            task_();
        }

        std::future<Result> getFuture() {
            return task_.get_future();
        }

    private:
        Result invokeTask() {
            return std::apply(
                [this](auto&... storedArgs) -> Result {
                    return std::invoke(func_, std::move(storedArgs)...);
                },
                args_);
        }

        Func func_;
        std::tuple<Args...> args_;
        std::packaged_task<Result()> task_;
    };

    template <typename Result>
    struct NewTask {
        std::unique_ptr<TaskBase> task;
        std::future<Result> future;
    };

public:
    explicit ThreadPool(
        std::size_t threadCount = static_cast<std::size_t>(
            std::thread::hardware_concurrency()),
        std::size_t maxQueueSize = 0)
        : maxQueueSize_(maxQueueSize) {
        if (threadCount == 0) {
            threadCount = 1;
        }

        try {
            workers_.reserve(threadCount);
            for (std::size_t i = 0; i < threadCount; ++i) {
                workers_.emplace_back([this] { workerLoop(); });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_ = true;
            }
            cvTasks_.notify_all();
            cvNotFull_.notify_all();

            for (std::thread& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw;
        }
    }

    ~ThreadPool() {
        shutdown(ShutdownMode::Drain);
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <typename Func, typename... Args>
    [[nodiscard]] std::future<InvokeResult<Func, Args...>> submit(
        Func&& func, Args&&... args) {
        std::unique_lock<std::mutex> lock(mutex_);
        cvNotFull_.wait(lock, [this] {
            return stop_ || maxQueueSize_ == 0 ||
                   tasks_.size() < maxQueueSize_;
        });

        if (stop_) {
            lock.unlock();
            throw std::runtime_error("submit on a shutdown ThreadPool");
        }

        const std::uint64_t taskId = totalSubmitted_ + 1;
        NewTask<InvokeResult<Func, Args...>> newTask =
            makeTask(taskId, std::forward<Func>(func), std::forward<Args>(args)...);
        prepareCompletionFlagLocked(taskId);
        tasks_.push_back(std::move(newTask.task));
        totalSubmitted_ = taskId;
        std::future<InvokeResult<Func, Args...>> future =
            std::move(newTask.future);

        lock.unlock();
        cvTasks_.notify_one();
        return future;
    }

    template <typename Func, typename... Args>
    [[nodiscard]] std::optional<std::future<InvokeResult<Func, Args...>>>
    try_submit(Func&& func, Args&&... args) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_ || (maxQueueSize_ > 0 && tasks_.size() >= maxQueueSize_)) {
            return std::nullopt;
        }

        const std::uint64_t taskId = totalSubmitted_ + 1;
        NewTask<InvokeResult<Func, Args...>> newTask =
            makeTask(taskId, std::forward<Func>(func), std::forward<Args>(args)...);
        prepareCompletionFlagLocked(taskId);
        tasks_.push_back(std::move(newTask.task));
        totalSubmitted_ = taskId;
        std::future<InvokeResult<Func, Args...>> future =
            std::move(newTask.future);

        lock.unlock();
        cvTasks_.notify_one();
        return std::optional<std::future<InvokeResult<Func, Args...>>>(
            std::move(future));
    }

    void shutdown(ShutdownMode mode = ShutdownMode::Drain) {
        std::deque<std::unique_ptr<TaskBase>> discardedTasks;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stop_) {
                stop_ = true;
                if (mode == ShutdownMode::Discard) {
                    for (const auto& task : tasks_) {
                        markTaskCompletedLocked(task->id);
                    }
                    discardedTasks.swap(tasks_);
                }
            }
        }

        // Destroy discarded packaged tasks only after releasing mutex_.
        discardedTasks.clear();

        // Notify only after the lock has been released.
        cvTasks_.notify_all();
        cvWait_.notify_all();
        cvNotFull_.notify_all();

        std::call_once(shutdownOnce_, [this] {
            for (std::thread& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        });
    }

    void shutdown(bool drain) {
        shutdown(drain ? ShutdownMode::Drain : ShutdownMode::Discard);
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::uint64_t target = totalSubmitted_;
#ifdef THREAD_POOL_TEST_ON_WAIT_ENTERED
        // Test-only hook: invoked after the snapshot is captured and before
        // waiting. Production builds compile this out.
        THREAD_POOL_TEST_ON_WAIT_ENTERED();
#endif
        cvWait_.wait(lock, [this, target] {
            return nextCompletionId_ > target;
        });
    }

    bool isShutdown() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_;
    }

    std::size_t pendingTasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    std::size_t activeTasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return activeTasks_;
    }

    std::size_t threadCount() const noexcept {
        return workers_.size();
    }

private:
    template <typename Func, typename... Args>
    NewTask<InvokeResult<Func, Args...>> makeTask(
        std::uint64_t id, Func&& func, Args&&... args) {
        // submit() and try_submit() currently construct the task while holding
        // mutex_. Before moving construction outside the critical section,
        // preserve bounded-queue failure semantics and quantify the change.
        using TaskType =
            PackagedTask<std::decay_t<Func>, std::decay_t<Args>...>;

        std::unique_ptr<TaskType> task = std::make_unique<TaskType>(
            std::forward<Func>(func), std::forward<Args>(args)...);
        task->id = id;
        std::future<InvokeResult<Func, Args...>> future = task->getFuture();
        return {std::move(task), std::move(future)};
    }

    void prepareCompletionFlagLocked(std::uint64_t id) {
        if (id < nextCompletionId_) {
            return;
        }

        const auto offset = static_cast<std::size_t>(id - nextCompletionId_);
        if (offset >= completionFlags_.size()) {
            completionFlags_.resize(offset + 1, 0);
        }
    }

    void markTaskCompletedLocked(std::uint64_t id) noexcept {
        if (id < nextCompletionId_) {
            return;
        }

        const auto offset = static_cast<std::size_t>(id - nextCompletionId_);
        if (offset >= completionFlags_.size()) {
            return;
        }

        completionFlags_[offset] = 1;
        while (!completionFlags_.empty() && completionFlags_.front() != 0) {
            completionFlags_.pop_front();
            ++nextCompletionId_;
        }
    }

    void workerLoop() noexcept {
        for (;;) {
            std::unique_ptr<TaskBase> task;
            std::uint64_t taskId = 0;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cvTasks_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });

                if (tasks_.empty()) {
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop_front();
                taskId = task->id;
                ++activeTasks_;
            }

            cvNotFull_.notify_one();

            try {
                task->run();
            } catch (...) {
                // packaged_task normally stores user exceptions; this is a
                // final guard for unexpected failures in task execution.
            }

            task.reset();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --activeTasks_;
                markTaskCompletedLocked(taskId);
            }
            cvWait_.notify_all();
        }
    }

    mutable std::mutex mutex_;
    // Keep task availability and wait() completion separate so submit()'s
    // notify_one() cannot wake a wait() caller instead of an idle worker.
    std::condition_variable cvTasks_;
    std::condition_variable cvWait_;
    std::condition_variable cvNotFull_;
    std::deque<std::unique_ptr<TaskBase>> tasks_;
    std::vector<std::thread> workers_;
    std::once_flag shutdownOnce_;
    std::size_t maxQueueSize_ = 0;
    std::uint64_t totalSubmitted_ = 0;
    std::uint64_t nextCompletionId_ = 1;
    std::deque<std::uint8_t> completionFlags_;
    std::size_t activeTasks_ = 0;
    bool stop_ = false;
};

}  // namespace mylib

#endif  // MYLIB_THREAD_POOL_HPP
