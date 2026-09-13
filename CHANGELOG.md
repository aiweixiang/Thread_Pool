# Changelog

## 未发布

### 新增
- 完成 C++17 header-only `mylib::ThreadPool` 核心实现。
- `submit` / `try_submit` 支持普通函数、lambda、函数对象、成员函数指针、
  move-only 可调用对象、move-only 参数、返回值与 `void`。
- 支持无界队列和有界队列；有界队列满时 `submit` 阻塞，`try_submit` 返回
  `std::nullopt`。
- 新增 `ShutdownMode::Drain` / `ShutdownMode::Discard`，以及
  `wait`、`isShutdown`、`pendingTasks`、`activeTasks`、`threadCount`。
- 新增 demo、无第三方测试框架的单元测试和 CMake/CTest 构建配置。
- CMake 增加 TSan、ASan、UBSan 开关。

### 修复
- R1：`submit`、`try_submit`、`makeTask` 统一使用
  `std::invoke_result_t<Func&, Args...>` 推导返回类型。
- R2：`try_submit` 先检查关闭状态和队列容量，失败时不消费入参。
- R3：`shutdown(Discard)` 将被丢弃任务换出后，在锁外析构。
- R4：所有 condition variable 通知均移到解锁之后。
- R5：`submit` 和 `try_submit` 标记 `[[nodiscard]]`。

### 兼容性
- 保留 `shutdown(bool)`：`true` 表示 `Drain`，`false` 表示 `Discard`。
- 并发 `shutdown` 仍由第一个切换停止状态的调用者决定模式，并通过
  `std::call_once` 保证所有调用者在 worker join 完成后返回。
