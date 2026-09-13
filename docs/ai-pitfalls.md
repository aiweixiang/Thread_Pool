# 规则集

> 改 thread_pool.hpp 前后各读一遍。只列规则，不解释。
> 为什么见 pitfalls.md。

## 本轮新雷（必须避开）

- R1 返回类型按左值推导：std::invoke_result_t<Func&, Args...>。
  submit / try_submit / makeTask 三处必须一致。
- R2 try_submit 失败不得消费入参，或在头文件注释里声明
  「失败时入参可能已被移动」。
- R3 shutdown(Discard) 的被丢弃任务在锁外析构：
  swap 到锁外局部变量，锁释放后再让其析构。
- R4 notify 放在解锁之后，不要在持锁时 notify_all。
- R5 submit / try_submit 加 [[nodiscard]]。

## 历史雷（不要退化）

- 调用处不对 func 做无意义的 std::move（packaged_task 只调用一次）。
- 任务内不递归 submit() 回同一池 → 队列满时死锁。
- 任务内不调用本池 shutdown() / wait() / 析构。
- 并发 shutdown()：先到者的 mode 生效；join 用 std::call_once；
  所有调用者等到 join 完成才返回。
- 构造函数异常：置 stop_ → notify → join 已建线程 → 重新抛出。
- workerLoop 对 task() 包 try/catch(...)。
- 锁内不得让异常逃逸（submit 显式抛 runtime_error 除外）。

## 头文件必须保留的两段

使用禁忌
- 任务内不递归 submit()；需要时用 try_submit()。
- 任务内不调用本池 shutdown() / wait() / 析构。

已知语义
- shutdown(Discard) 后，被丢弃任务的 future 抛 broken_promise。
- 并发 shutdown() 参数不同时，只有第一个切换 stop_ 的 mode 生效。
- wait() 只保证调用时刻已提交任务清空，不保证之后仍可提交。
