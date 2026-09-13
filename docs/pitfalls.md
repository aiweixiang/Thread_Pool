# 雷点台账

> 每条含：现象 / 根因 / 规则 / 状态。规则集见 ai-pitfalls.md，这里只讲为什么。

---

## P1 invoke_result_t 与调用处值类别不一致（本轮新雷）

现象：submit / makeTask 用 invoke_result_t<Func, Args...> 推导，
但实际调用 std::invoke(func, ...) 里 func 是左值。可调用对象同时有
operator()& 和 operator()&& 且返回类型不同时，推导类型与实际调用不一致，
轻则窄化，重则编译失败。

根因：推导用右值值类别，调用用左值。

规则：一律用 std::invoke_result_t<Func&, Args...>，三处一致。

状态：待修复。

---

## P2 try_submit 失败时已移动入参（本轮新雷）

现象：先构造 task 再检查队列满/关闭。返回 nullopt 时 move-only 参数
已被移入 task 并销毁，调用者手里的对象被置空。

根因：构造与检查顺序颠倒。

规则：二选一：先加锁检查再构造 task；或保留现顺序但在头文件注释声明
「失败时入参可能已被移动」。

状态：待定。

---

## P3 shutdown(Discard) 持锁析构被丢弃任务（本轮新雷）

现象：锁作用域内 swap 出的队列在锁内析构，任务捕获的昂贵对象析构
会拉长锁持有时间，阻塞并发 submit。

根因：被丢弃队列生命周期被锁作用域限制。

规则：swap 到锁外局部变量，锁释放后再析构。

状态：待修复。

---

## P4 锁内 notify（本轮新雷）

现象：持锁时 notify_all，被唤醒线程立刻撞锁。

根因：通知在锁内。

规则：先改状态、解锁，再 notify。

状态：待优化。

---

## P5 返回值可被静默丢弃（本轮新雷）

现象：忽略 submit 返回的 future 会在队列满时莫名阻塞；忽略
try_submit 的 optional 会吞掉失败。

规则：两者都加 [[nodiscard]]。

状态：待添加。

---

## P6 任务内递归 submit 回同一池（历史）

队列满时，所有 worker 都在执行「内部 submit 且阻塞在 cvNotFull_」的任务，
没人消费队列 → 死锁。规则：任务内需要提交时用 try_submit。

状态：文档已覆盖，须保留。

---

## P7 任务内 shutdown / wait / 析构本池（历史）

任务内 shutdown 会在 call_once 里 join 自己 → std::system_error；
任务内 wait 自身计入 activeCount_ → 死等。

状态：文档已覆盖，须保留。

---

## P8 并发 shutdown 参数取舍（历史）

只有第一个把 stop_ 从 false 置 true 的调用者 mode 生效，其余被忽略。
join 用 std::call_once，所有调用者等到 join 完成才返回。

状态：文档已覆盖，须保留。

---

## P9 shutdown(Discard) 后 future 抛 broken_promise（历史）

被丢弃任务的 packaged_task 随 shared_ptr 析构，未调用即销毁。

状态：文档已覆盖，须保留。

---

## P10 构造函数线程创建失败导致 terminate（历史）

vector<thread> 析构时若有 joinable 线程未 join → terminate。
规则：置 stop_ → notify → join 已建线程 → 重新抛出。

状态：已修复，回归测试须保留。

---

## P11 workerLoop 未捕获非用户异常（历史）

std::bad_function_call 等会杀死工作线程。规则：task() 外包
try/catch(...) 兜底。

状态：已修复，回归测试须保留。
