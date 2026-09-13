# Repository Rules

适用版本：`v0.1.1` 及之后。修改代码前先读本文件，再读 `docs/pitfalls.md`。

## 不可破坏的行为

- 保持 C++17、header-only、无第三方运行时依赖。
- `submit()` 返回 `std::future<R>`；`try_submit()` 只在队列满或池关闭时返回
  `std::nullopt`，失败不得消费入参。
- `wait()` 只等待调用时刻前已成功提交的任务，必须保留快照语义。
- 并发 `shutdown()` 由第一个改变停止状态的调用者决定模式；所有调用者等到
  worker join 完成后返回。
- `shutdown(Discard)` 的清理由锁外执行；所有 condition variable 通知在解锁后发送。
- worker 任务可用性使用 `cvTasks_`，`wait()` 使用独立的 `cvWait_`。
- `PackagedTask` 捕获 `this`，复制和移动必须保持删除状态。若要让任务按值搬动，
  必须先把任务改成 `std::promise<Result>`；不要修现有移动构造。
- 任务构造失败时不得推进 `totalSubmitted_`，也不得留下阻塞完成前缀的空洞。
- `submit()` 显式抛出的关闭异常必须继续兼容 `catch (const std::runtime_error&)`。

## 必须保留的公开语义

- `isShutdown()` 只表示关闭已经开始，不表示任务结束或 worker 已 join。
- `shutdown(Drain)` 执行完排队任务；`shutdown(Discard)` 只取消未开始任务。
- `std::ref` 不延长对象生命周期；任务异常只能通过 `future.get()` 观测。
- 销毁线程池前，所有可能调用池成员函数的外部线程必须已经 join。

## 修改后

- 用户可见行为变化同步更新 `README.md`、`thread_pool.hpp` 顶部注释和
  `docs/requirements.md`。
- 新增或改变风险状态时，只更新 `docs/pitfalls.md`；不要另建规则编号或状态清单。
- 提交前执行 `docs/review-checklist.md`，并为用户可见变化更新 `CHANGELOG.md`。
