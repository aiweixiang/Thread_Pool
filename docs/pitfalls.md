# 风险与规则台账

> 这是项目唯一的风险事实源。规则只写行为，原因和状态只维护在这里。
> 已解决的旧条目可以压缩，但 P 编号永久保留，不得复用或重排。

状态约定：

- **Fixed**：代码已修复，并有不变量或回归测试保护。
- **Guarded**：当前实现安全，但依赖明确的使用前提或防误用措施。
- **Open**：尚未处理，或只完成了文档/测试的一部分。
- **Planned**：已确认需要处理，但没有阻塞当前版本。

## 改代码前必须遵守

1. 保持 C++17、header-only、无第三方运行时依赖。
2. 返回类型使用 `std::invoke_result_t<std::decay_t<Func>&, std::decay_t<Args>...>`；
   `submit()`、`try_submit()` 和 `makeTask()` 必须一致。
3. `try_submit()` 先检查队列容量和关闭状态，失败返回 `nullopt` 时不得消费入参。
4. 完成任务前先成功构造任务，再登记完成位，最后入队；任务未接受不得推进 ID。
5. 锁内不执行 `notify`；`Discard` 的被丢弃任务必须在锁外析构。
6. worker 等待使用 `cvTasks_`，`wait()` 使用独立的 `cvWait_`。
7. `wait()` 保持“等待调用时刻前已成功提交任务”的快照语义。
8. 并发 `shutdown()` 由第一个改变停止状态的调用者决定模式；用 `call_once` 保证
   所有调用者都等到 worker join 完成。
9. worker 执行任务必须有最终 `catch (...)`；构造函数创建线程失败时必须先停止、
   notify，再 join 已建线程并重新抛出。
10. `PackagedTask` 捕获 `this`，复制和移动保持删除。若要让任务按值搬动，必须先把
    任务改成 `std::promise<Result>`，不要修现有移动构造。
11. 用户可调用对象和参数目前在线程池锁内构造；其构造/析构代码不得回调本池。
12. `submit()` 的关闭异常继续保持 `std::runtime_error` 兼容性；`isShutdown()` 只表示
    关闭已经开始。
13. 锁内路径不得让异常逃逸；显式关闭异常和用户构造异常除外。
14. 调用处不要对 `func` 做无意义的 `std::move`；任务只执行一次。

## 活跃与部分解决风险

### P17 自引用的 PackagedTask 可移动会造成 UAF

**状态：Guarded / 第 4 轮前置条件**

- 风险：`packaged_task` 捕获 `this`。若任务对象被搬动，源对象析构后任务会访问悬垂地址。
- 当前保护：复制、移动和对应赋值全部 `= delete`，且只通过 `unique_ptr` 持有。
- 未来规则：允许按值搬动前，改用 `std::promise<Result>` 并由 `run()` 直接写值。
- 完成信号：promise 改造通过 ASan 压力测试后，才允许引入内联存储或按值队列。

### P18 持锁构造任务会放大锁竞争和重入风险

**状态：Open（性能与扩展性）**

- 现状：`submit()` / `try_submit()` 在全局锁内分配任务、构造 `packaged_task` 并复制参数。
- 风险：慢构造会串行化提交；构造函数回调本池会自死锁。
- 约束：优化前先基准量化，并保留“有界队列失败不消费入参”和“失败不烧 ID”语义。
- 归属：第 4 轮，不能在 P17 的 promise 改造前做任务存储重构。

### P19 关闭异常与 try_submit 失败原因不可区分

**状态：Open（API 增强，可选）**

- 已关闭池上的 `submit()` 与用户构造异常都可能是 `std::runtime_error`。
- `try_submit()` 的 `nullopt` 可能表示队列满或池已关闭。
- 当前规则：保留兼容类型，文档要求用 `isShutdown()` 辅助区分。
- 可选增强：新增 `ThreadPoolClosed`，并提供带失败原因输出的 `try_submit()` 重载。

### P20 析构与其他成员调用并发会访问已释放对象

**状态：Guarded（调用方生命周期前提）**

- `shutdown()` 或析构会唤醒阻塞中的 submitter，但不等待其他成员调用全部返回。
- 当前规则：销毁前必须 join 所有可能调用 `submit()`、`try_submit()`、`wait()` 或
  `shutdown()` 的外部线程。
- 专项 ASan 30 轮未复现；不要把它当成可直接修的实现 bug 或擅自改 shutdown 语义。

### P24 默认无界队列缺少背压

**状态：Open（文档优先，默认值不改）**

- 默认 `maxQueueSize == 0` 表示无界；生产速度长期高于消费速度时可能持续增长直至 OOM。
- 建议：README 明确内存风险，生产环境优先使用有界队列；不破坏现有默认行为。

### P25 任务构造异常缺少契约与回归保险

**状态：Open（测试增强）**

- 用户复制/移动构造、分配或容器操作可能在任务入队前抛异常。
- 期望不变量：任务未被接受、`totalSubmitted_` 不推进、锁正常释放、池仍可继续使用。
- 需要测试：构造抛出 `runtime_error` 后，立即提交正常任务并验证可以完成。
- 文档：`try_submit()` 返回 `nullopt` 只代表队列满/池关闭，不代表构造绝不抛异常。

### P26 并发 API 组合覆盖不足

**状态：Open（测试增强）**

- 当前重点覆盖 `submit()` × `shutdown()`，`try_submit()`、`wait()` 与多提交者的组合不足。
- 需要补充接受任务数、`fulfilled + broken_promise` 总数和带超时就绪检查。
- 与 P28、P32 同批处理，避免只验证“不崩溃”而漏掉任务丢失。

### P27 shutdown 和析构可能无限等待

**状态：Open（文档边界，不增加取消机制）**

- `Drain` 和析构等待所有任务结束；`Discard` 也不中断活动任务。
- 活动任务永久阻塞时，shutdown 和析构会永久阻塞。
- 当前规则：README 必须明确任务必须能自行结束；超时、强杀和取消属于新设计。

### P28 wait 快照测试存在调度盲区

**状态：Open（测试增强）**

- 现有测试不能严格证明 waiter 已进入 `wait()` 后再提交 later 任务。
- 目标：增加测试专用观测机制或确定性 hook，不改变公开 API 和快照语义。

### P29 安装后缺少完整 CMake package

**状态：Open（打包能力）**

- 当前导出 `ThreadPoolTargets.cmake`，但没有 `ThreadPoolConfig.cmake` / ConfigVersion。
- 需要时增加 `find_dependency(Threads)`、namespace alias 和安装后 consumer 测试。
- 同时提供 `THREAD_POOL_BUILD_TESTS` / `THREAD_POOL_BUILD_DEMO`，避免子项目污染。

### P30 完成位图存在内存增长与 32 位偏移风险

**状态：Open（先测后改）**

- 队首长期未完成时，后续完成位必须保留；一亿任务约 100 MB。
- 若改区间压缩或分块位图，必须处理 32 位平台的累计任务数到 `size_t` 截断。
- 当前 `wait()` 快照语义正确，不是泄漏；改实现前先增加内存压力用例。

### P31 cvWait_ 在完成前缀未推进时仍 notify_all

**状态：Open（性能优化）**

- 长头任务存在时，短尾任务完成会产生无效惊群。
- 低风险方向：仅在 `nextCompletionId_` 真正推进时通知；`shutdown()` 的通知必须保留。
- 与 P18/P30 一样，benchmark 后再决定是否实施。

### P32 并发 shutdown 测试无法发现任务丢失

**状态：Open（测试增强）**

- 现有测试丢弃所有 future，无法区分任务执行、broken_promise 和彻底丢失。
- 必须收集 future，使用 `wait_for(2s)` 超时检查，并断言
  `fulfilled + broken == accepted` 且 `fulfilled == executed`。

### P33 CI 与警告约束仍有缺口

**状态：Open（CI 加固）**

- 当前没有 `-Werror`，demo 只编译不运行，也没有 clang / MSVC job。
- `failures` 非原子，缺少编译期负向测试。
- CI 可增加 `timeout-minutes`、旧 run 自动取消，并评估固定 `actions/checkout` SHA。

## 已修复或已有明确约束的风险

| 编号 | 主题 | 状态 | 当前不变量 |
|---|---|---|---|
| P1 | 调用值与推导值类别 | Fixed | 统一按存储后的左值调用推导 |
| P2 | `try_submit()` 失败消费入参 | Fixed | 失败路径不构造任务、不移动参数 |
| P3 | Discard 持锁析构 | Fixed | 被丢弃任务在锁外析构 |
| P4 | 锁内通知 | Fixed | 先改状态、解锁，再 notify |
| P5 | 返回值静默丢弃 | Fixed | 两个提交接口均 `[[nodiscard]]` |
| P6 | 任务递归提交同池 | Guarded | 任务内不递归阻塞提交 |
| P7 | 任务内 shutdown/wait/析构 | Guarded | 任务内禁止调用池生命周期 API |
| P8 | 并发 shutdown 模式 | Fixed | 第一个模式生效，所有调用者等 join |
| P9 | Discard 后 future | Fixed | 被丢弃任务的 future 报 `broken_promise` |
| P10 | 构造 worker 失败 | Fixed | 停止、notify、join 已建线程后再抛 |
| P11 | worker 未捕获异常 | Fixed | 任务执行外包最终 `catch (...)` |
| P12 | wait 与 worker 共用 CV | Fixed | 任务可用和完成等待使用不同 CV |
| P13 | `deque<bool>` 代理引用 | Fixed | 完成位图使用 `deque<uint8_t>` |
| P14 | prepare 早于 makeTask | Fixed | 构造成功后才登记完成位 |
| P15 | 同池 future 依赖 | Guarded | 不在任务内等待同池未完成任务 |
| P16 | `isShutdown()` 被误读 | Guarded | 仅表示关闭已开始 |
| P21 | `std::ref` 生命周期 | Guarded | 引用对象必须活到任务结束 |
| P22 | future 异常不可观测 | Guarded | 必须调用 `get()` 才能获得异常 |
| P23 | 文档使用相对轮次 | Fixed | 标题按版本或主题命名 |
