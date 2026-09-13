# 规则集

> 改 thread_pool.hpp 前后各读一遍。只列规则，不解释。
> 为什么见 pitfalls.md。

## v0.1.0 基线雷点（R1–R8）

- R1 返回类型按左值推导：std::invoke_result_t<Func&, Args...>。
  submit / try_submit / makeTask 三处必须一致。
- R2 try_submit 失败不得消费入参，或在头文件注释里声明
  「失败时入参可能已被移动」。
- R3 shutdown(Discard) 的被丢弃任务在锁外析构：
  swap 到锁外局部变量，锁释放后再让其析构。
- R4 notify 放在解锁之后，不要在持锁时 notify_all。
- R5 submit / try_submit 加 [[nodiscard]]。
- R6 worker 任务可用和 wait() 完成等待使用独立 condition variable，
  避免 notify_one() 唤醒错误等待者。
- R7 完成位图禁用 `deque<bool>`，使用 `deque<uint8_t>`。
- R8 先 `makeTask` 成功，再 `prepareCompletionFlagLocked`，再入队。

## 边界加固规则（R9 起，勿与外部风险编号混用）

- R9 任务不得同步等待同一线程池中尚未完成的其他任务；任务依赖在池外调度。
- R10 `isShutdown()` 只表示关闭已开始，不表示任务结束、worker 已 join、
  资源已释放。不要扩展或改变其现有语义。
- R11 `PackagedTask` 捕获 `this`，复制/移动必须保持 `= delete`。若后续要让任务
  按值搬动，先把 `packaged_task` + 捕获 `this` 的 lambda 改为 `std::promise`
  成员并直接写结果；不要修移动构造函数。
- R12 `submit()` / `try_submit()` 目前在持锁状态构造任务。第 4 轮基准量化前不要
  搬出临界区；任何优化必须保住有界队列失败时不消费入参的语义。
- R13 `submit()` 关闭异常与用户构造异常都是 `std::runtime_error`；
  `try_submit()` 的 `nullopt` 也可能是队列满或池关闭。保留现有返回/异常类型，
  文档要求用 `isShutdown()` 辅助区分。
- R14 销毁线程池前必须 join 所有可能调用 `submit()` / `try_submit()` /
  `wait()` / `shutdown()` 的线程。`shutdown()` 返回不保证这些调用已经返回。
- R15 `std::ref` 不延长生命周期；不调用 future.get() 会丢失任务异常。

## 历史雷（不要退化）

- 调用处不对 func 做无意义的 std::move（packaged_task 只调用一次）。
- 任务内不递归 submit() 回同一池 → 队列满时死锁。
- 任务内不调用本池 shutdown() / wait() / 析构。
- 任务不得等待同池 future；销毁前不得有并发成员调用。
- `std::ref` 的引用对象必须比任务执行更长寿；future 是异常观测通道。
- 并发 shutdown()：先到者的 mode 生效；join 用 std::call_once；
  所有调用者等到 join 完成才返回。
- 构造函数异常：置 stop_ → notify → join 已建线程 → 重新抛出。
- workerLoop 对 task() 包 try/catch(...)。
- 锁内不得让异常逃逸（submit 显式抛 runtime_error 除外）。

## 头文件必须保留的两段

使用禁忌
- 任务内不递归 submit()；需要时用 try_submit()。
- 任务内不调用本池 shutdown() / wait() / 析构。
- 任务内不同步等待同池中尚未完成的其他任务。
- 提交锁内构造对象不得回调本池；销毁前不得有并发成员调用。
- `std::ref` 引用对象必须存活到任务结束。

已知语义
- shutdown(Discard) 后，被丢弃任务的 future 抛 broken_promise。
- 并发 shutdown() 参数不同时，只有第一个切换 stop_ 的 mode 生效。
- wait() 只保证调用时刻已提交任务清空，不保证之后仍可提交。
- isShutdown() 只表示关闭已开始；try_submit() 失败原因可用它辅助区分。
- submit() 关闭异常与用户构造异常同为 runtime_error；不取 future 会丢失异常。
