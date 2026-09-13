# 雷点台账

> 每条含：现象 / 根因 / 规则 / 状态。规则集见 ai-pitfalls.md，这里只讲为什么。

---

## P1 invoke_result_t 与调用处值类别不一致

现象：submit / makeTask 用 invoke_result_t<Func, Args...> 推导，
但实际调用 std::invoke(func, ...) 里 func 是左值。可调用对象同时有
operator()& 和 operator()&& 且返回类型不同时，推导类型与实际调用不一致，
轻则窄化，重则编译失败。

根因：推导用右值值类别，调用用左值。

规则：一律用 std::invoke_result_t<Func&, Args...>，三处一致。

状态：已修复。

---

## P2 try_submit 失败时已移动入参

现象：先构造 task 再检查队列满/关闭。返回 nullopt 时 move-only 参数
已被移入 task 并销毁，调用者手里的对象被置空。

根因：构造与检查顺序颠倒。

规则：二选一：先加锁检查再构造 task；或保留现顺序但在头文件注释声明
「失败时入参可能已被移动」。

状态：已修复。

---

## P3 shutdown(Discard) 持锁析构被丢弃任务

现象：锁作用域内 swap 出的队列在锁内析构，任务捕获的昂贵对象析构
会拉长锁持有时间，阻塞并发 submit。

根因：被丢弃队列生命周期被锁作用域限制。

规则：swap 到锁外局部变量，锁释放后再析构。

状态：已修复。

---

## P4 锁内 notify

现象：持锁时 notify_all，被唤醒线程立刻撞锁。

根因：通知在锁内。

规则：先改状态、解锁，再 notify。

状态：已修复。

---

## P5 返回值可被静默丢弃

现象：忽略 submit 返回的 future 会在队列满时莫名阻塞；忽略
try_submit 的 optional 会吞掉失败。

规则：两者都加 [[nodiscard]]。

状态：已修复。

---

## P6 任务内递归 submit 回同一池

队列满时，所有 worker 都在执行「内部 submit 且阻塞在 cvNotFull_」的任务，
没人消费队列 → 死锁。规则：任务内需要提交时用 try_submit。

状态：文档已覆盖，须保留。

---

## P7 任务内 shutdown / wait / 析构本池

任务内 shutdown 会在 call_once 里 join 自己 → std::system_error；
任务内 wait 自身计入 activeCount_ → 死等。

状态：文档已覆盖，须保留。

---

## P8 并发 shutdown 参数取舍

只有第一个把 stop_ 从 false 置 true 的调用者 mode 生效，其余被忽略。
join 用 std::call_once，所有调用者等到 join 完成才返回。

状态：文档已覆盖，须保留。

---

## P9 shutdown(Discard) 后 future 抛 broken_promise

被丢弃任务的 packaged_task 随 unique_ptr 析构，未调用即销毁。

状态：文档已覆盖，须保留。

---

## P10 构造函数线程创建失败导致 terminate

vector<thread> 析构时若有 joinable 线程未 join → terminate。
规则：置 stop_ → notify → join 已建线程 → 重新抛出。

状态：已修复，回归测试须保留。无法在不侵入 API 的情况下模拟
`std::thread` 构造失败，回归依赖代码路径审查。

---

## P11 workerLoop 未捕获非用户异常

std::bad_function_call 等会杀死工作线程。规则：task() 外包
try/catch(...) 兜底。

状态：已修复，回归测试须保留。

---

## P12 wait() 与 worker 共用任务条件变量

现象：存在 `wait()` 等待者时，`submit()` 的 `notify_one()` 可能唤醒等待
完成状态的调用者，而不是空闲 worker；队列中有任务但无人执行。

根因：一个 condition variable 被用于两个不同的等待谓词。

规则：worker 等待任务使用 `cvTasks_`，`wait()` 使用独立的 `cvWait_`；
通知必须发送到对应条件变量。

状态：已修复。

---

## P13 `deque<bool>` 代理引用

现象：`operator[]` 返回代理引用而不是 `bool&`，后续改位图或取地址容易
踩坑。

根因：`std::deque<bool>` 是为节省空间实现的特化容器。

规则：完成位图使用 `std::deque<std::uint8_t>`，只写入 0/1。

状态：已修复。

---

## P14 先 prepare 再 makeTask

现象：任务构造失败时会留下永远为 false 的空洞完成位，前缀推进存在卡住
隐患。

根因：完成位登记早于任务构造成功。

规则：先 `makeTask` 成功，再 `prepareCompletionFlagLocked`，再入队。

状态：已修复。

---

## P15 同池任务通过 future 建立依赖

现象：先提交的任务同步等待后提交任务的 future。worker 数量不足时，等待中的
任务占住 worker，后提交任务没有机会执行，形成永久死锁。

根因：线程池只负责调度，不分析任务之间的依赖关系。

规则：任务不得同步等待同一线程池中尚未完成的其他任务；依赖在池外调度，
或使用不会占满 worker 的独立机制。

状态：头文件与 README 已覆盖，不实现依赖图调度。

---

## P16 把 isShutdown() 当成关闭完成

现象：`stop_` 切换为 true 后 `isShutdown()` 立即返回 true；此时活动任务可能
仍未结束，worker 也可能尚未 join。

根因：公开名称只描述“已开始关闭”，没有描述完整生命周期结束。

规则：文档明确 `isShutdown()` 只表示关闭已经开始；不改变现有语义。

状态：头文件与 README 已覆盖。

---

## P17 自引用的 PackagedTask 被隐式移动

现象：`task_` 内的 lambda 捕获 `this`，但类型未被禁止移动。对象一旦按值搬动，
移动后的 `task_` 仍回调源对象地址；源对象析构后 `run()` 会 use-after-free。

根因：`packaged_task` 的共享状态不能重新绑定到新对象地址，默认移动只搬走
`packaged_task`，不会改写其中保存的 `this`。

规则：当前保持复制和移动为 `= delete`，且只通过 `unique_ptr` 持有。若后续
要让任务对象可移动，使用 `std::promise<Result>` 成员替掉 `packaged_task` +
捕获 `this` 的 lambda；不要尝试修移动构造函数。

状态：已加 `= delete` 防护。当前没有对象搬动路径；完成 `promise` 改造前，
禁止引入内联任务存储或按值搬动。

---

## P18 持锁构造任务及构造期回调

现象：`submit()` / `try_submit()` 在 `mutex_` 保护下分配任务、构造
`packaged_task` 并复制/移动用户对象。慢构造会串行化提交；如果用户对象的
构造函数回调同一线程池，非递归互斥锁会自死锁。

根因：任务构造仍在全局锁的临界区内。

规则：当前只注释并记录该边界，不提前改实现。第 4 轮先量化，再评估把构造移出
临界区；有界队列必须继续保持失败时不消费入参。

状态：代码注释与文档已覆盖，性能优化待第 4 轮。

---

## P19 关闭异常与 try_submit 失败原因不可区分

现象：已关闭池上的 `submit()` 抛 `std::runtime_error`，但用户参数或可调用对象
构造也可能抛同类异常；`try_submit()` 返回 `nullopt` 时无法仅凭返回值判断队列满
还是池已关闭。

根因：现有接口没有独立错误类型或失败原因输出。

规则：保留当前异常/返回类型兼容性，文档要求捕获后可用 `isShutdown()` 辅助区分，
并明确并发竞态下仍可能歧义。新增 `ThreadPoolClosed` 属于可选后续增强，不纳入当前迭代。

状态：头文件与 README 已明确歧义。

---

## P20 析构与其他成员调用并发

现象：析构可以唤醒被阻塞的 submitter，但被唤醒线程可能仍在成员函数内部；析构
返回并释放对象后，该线程继续访问成员就会悬垂。`shutdown()` 返回也不保证其他
并发调用已经返回。

根因：对象生命周期依赖调用方保证“最后一个外部调用先返回”，API 本身没有内部
调用计数来消除该前提。

规则：销毁前先 join 所有可能调用 `submit()` / `try_submit()` / `wait()` /
`shutdown()` 的线程。

状态：已写入头文件与 README。专项 ASan 复现 30 轮未触发；此项是文档前提，
不是待修 bug，不修改 shutdown 语义。

---

## P21 std::ref 不延长对象生命周期

现象：任务异步执行，但 `std::ref` 只保存引用；被引用对象先析构时，任务会访问
悬垂对象。

根因：线程池无法推断引用目标的实际生命周期。

规则：头文件与 README 明确要求被引用对象存活到任务执行结束。

状态：文档已覆盖。

---

## P22 放弃 future 后任务异常不可观测

现象：任务异常通常写入 future 的共享状态；如果调用方既不保存也不等待 future，
异常没有观测者，最终被静默丢弃。`workerLoop` 的最终 catch 也没有诊断通道。

根因：future 是当前唯一异常透传出口。

规则：文档明确不调用 `get()` 就无法获得任务异常；仅 `wait()` 不会重抛异常。

状态：头文件与 README 已覆盖。

---

## P23 过期“本轮”措辞与不对称标题

现象：规则集、checklist 和雷点台账使用“本轮新雷”指代已经结束的 v0.1.0 阶段；
同时 P1–P5/P12 带轮次后缀，后续 P 条目却没有，文件内部命名不一致。

根因：标题记录了相对时间，而文档会跨轮次长期维护。

规则：规则和雷点标题按版本或主题命名，不使用“本轮”；雷点标题统一只写主题，
状态由各自的“状态：”行表达。

状态：已修复。规则集和 checklist 改为 v0.1.0 基线雷点，P 条目标题统一去掉
过期轮次后缀。
