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
 *
 * 已知语义
 * - try_submit() 只有在确认队列未满且线程池未关闭后才构造任务，因此失败返回
 *   std::nullopt 时不会消费入参。
 * - shutdown(Discard) 后，被丢弃任务的 future 会以 std::future_error
 *   (broken_promise) 结束。
 * - 并发 shutdown() 参数不同时，只有第一个把 stop_ 从 false 切换为 true
 *   的调用者的模式生效；所有调用者都在 worker join 完成后才返回。
 * - wait() 只等待调用时刻之前已经成功提交的任务清空且不再执行，不保证之后
 *   不再提交任务；与 shutdown() 并发时，Discard 丢弃的任务也会被计入完成。
 */

#include <condition_variable>
#include <cstddef>
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
        std::size_t threadCount = std::thread::hardware_concurrency(),
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

        NewTask<InvokeResult<Func, Args...>> newTask =
            makeTask(std::forward<Func>(func), std::forward<Args>(args)...);
        tasks_.push_back(std::move(newTask.task));
        ++totalSubmitted_;
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

        NewTask<InvokeResult<Func, Args...>> newTask =
            makeTask(std::forward<Func>(func), std::forward<Args>(args)...);
        tasks_.push_back(std::move(newTask.task));
        ++totalSubmitted_;
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
                    discardedTasks.swap(tasks_);
                    completedTasks_ += discardedTasks.size();
                }
            }
        }

        // Destroy discarded packaged tasks only after releasing mutex_.
        discardedTasks.clear();

        // Notify only after the lock has been released.
        cvTasks_.notify_all();
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
        const std::size_t target = totalSubmitted_;
        cvTasks_.wait(lock, [this, target] {
            return completedTasks_ >= target;
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
        Func&& func, Args&&... args) {
        using TaskType =
            PackagedTask<std::decay_t<Func>, std::decay_t<Args>...>;

        std::unique_ptr<TaskType> task = std::make_unique<TaskType>(
            std::forward<Func>(func), std::forward<Args>(args)...);
        std::future<InvokeResult<Func, Args...>> future = task->getFuture();
        return {std::move(task), std::move(future)};
    }

    void workerLoop() noexcept {
        for (;;) {
            std::unique_ptr<TaskBase> task;

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
                ++completedTasks_;
            }
            cvTasks_.notify_all();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cvTasks_;
    std::condition_variable cvNotFull_;
    std::deque<std::unique_ptr<TaskBase>> tasks_;
    std::vector<std::thread> workers_;
    std::once_flag shutdownOnce_;
    std::size_t maxQueueSize_ = 0;
    std::size_t totalSubmitted_ = 0;
    std::size_t completedTasks_ = 0;
    std::size_t activeTasks_ = 0;
    bool stop_ = false;
};

}  // namespace mylib

#endif  // MYLIB_THREAD_POOL_HPP
