# 需求与验收标准

## 定位

- C++17，header-only，无第三方运行时依赖，命名空间 mylib。
- 面向通用场景，可直接引入中小型项目。

## 功能需求

F1 任务提交
- submit(f, args...) -> std::future<R>，R 按左值调用推导。
- 支持：普通函数 / lambda / 函数对象 / 成员函数指针；返回值与 void；
  move-only 参数；move-only 可调用对象。
- 任务异常（含非 std::exception，如 throw 42）通过 future.get() 透传。

F2 非阻塞提交
- try_submit(f, args...) -> std::optional<std::future<R>>。
- 队列满或已关闭时立即返回 nullopt，不阻塞。
- 失败时的入参语义必须在头文件注释中声明。

F3 有界队列
- maxQueueSize = 0 无界；> 0 有界。
- 有界且满时，submit() 阻塞，直到有空位或线程池关闭。
- 关闭期间被阻塞的 submit() 必须被唤醒（入队成功或抛异常）。

F4 生命周期
- shutdown(ShutdownMode)：
  - Drain 等排队任务跑完；Discard 丢弃排队任务，只等正在执行的完成。
  - 幂等、可并发；并发时第一个切换 stop_ 的调用者的 mode 生效。
  - 所有调用者都在所有 worker join 完成后才返回。
- shutdown(bool) 兼容旧调用点。
- wait() 阻塞到「调用时刻已提交的任务」清空且无任务执行，线程池继续存活。
- isShutdown() / pendingTasks() / activeTasks() / threadCount() 只读观测；
  isShutdown() 只表示关闭已经开始，不等待 worker join。
- 析构默认 shutdown(Drain)。

F5 异常安全
- 构造期间 worker 创建失败：join 已创建线程后再抛，不 terminate。
- workerLoop 对 task() 包 try/catch(...) 兜底。
- 锁内路径不得抛异常逃逸（submit 显式抛 runtime_error 除外）；文档须说明
  关闭异常与用户构造异常可能同为 runtime_error。

## 非功能需求

- N1 并发正确性：CI 必须运行 TSan；submit / try_submit / shutdown / wait
  可多线程并发调用。
- N2 可观测性：提供 pendingTasks() / activeTasks() / threadCount()。
- N3 文档：thread_pool.hpp 顶部含「能力 / 使用禁忌 / 已知语义」三段。
- N4 兼容性：不破坏 shutdown(bool)；不改 stop_ 切换与 call_once join 语义。
- N5 使用边界：文档须覆盖同池 future 依赖、持锁构造任务、析构并发前提、
  std::ref 生命周期，以及不调用 future.get() 时异常不可观测。

## 验收

    cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
    cmake -S . -B build-tsan -DTHREAD_POOL_ENABLE_TSAN=ON
    cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure

必须全绿，且测试至少覆盖：

- submit 返回值 / void / 异常透传（std::exception 与 int）；
- move-only 参数与 move-only 可调用对象；
- wait() 在 activeCount_ > 0 && tasks_.empty() 时仍阻塞；
- 有界队列满 + 阻塞 submit + shutdown(Discard) 交互；
- try_submit 队列满 / 已关闭返回 nullopt；
- 并发 shutdown() × N 与 submit() × M 不死锁、不崩溃；
- shutdown(Discard) 后被丢弃任务的 future 抛 broken_promise；
- shutdown 幂等（重复调用、参数不同）；
- 已关闭池上 submit 抛 runtime_error。

## 不做

任务优先级、定时任务、依赖图调度、无锁队列、C++20 stop_token 协作取消。
