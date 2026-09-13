# 交付前自检

> 每次改完逐条勾选，结果写进提交说明：通过 / 不适用 / 需讨论。
> 规则原文见 ai-pitfalls.md，本文件只列可执行检查项。

## 编译与测试
- [x] 构建无警告（-Wall -Wextra）。
- [x] ctest 全绿。
- [x] 并发改动跑过 TSan。
- [x] 跑过 ASan/UBSan。
- [x] 测试文件无 sleep_for。
- [x] cmake --install 能安装头文件和 ThreadPoolTargets.cmake。
- [x] CI YAML 包含 push、pull_request 和 test / tsan / asan 三个 job。

## 本轮新雷（对应 R1–R8）
- [x] R1：三处返回类型推导都用 Func&，一致。
- [x] R1：可调用对象同时有 operator()& / operator()&& 时返回类型正确。
- [x] R2：try_submit 失败时入参语义明确（未消费 或 注释声明）。
- [x] R3：shutdown(Discard) 被丢弃任务在锁外析构。
- [x] R4：所有 notify 在解锁之后。
- [x] R5：submit / try_submit 有 [[nodiscard]]。
- [x] R6：worker 与 wait() 使用独立 condition variable。
- [x] R7：完成位图使用 deque<uint8_t>，未使用 deque<bool>。
- [x] R8：先 makeTask 成功，再 prepare，再入队。

## 历史雷（不退化）
- [x] 调用处无无意义的 std::move(func)。
- [x] 头文件「使用禁忌」仍覆盖：递归 submit、任务内 shutdown/wait/析构。
- [x] 头文件「已知语义」仍覆盖：broken_promise、并发 shutdown 参数取舍、
      wait 与 shutdown 并发的边界，以及 threadCount() 语义。
- [x] shutdown(bool) 旧重载保留。
- [x] 并发 shutdown 语义未变（先到者 mode 生效，call_once join）。
- [x] 构造函数异常仍 join 已建线程再抛（代码路径审查）。
- [x] workerLoop 仍有 try/catch(...)。
- [x] 锁内资源由 RAII 管理，构造/分配异常不会导致死锁。
- [x] P10 构造 std::thread 失败测试：不适用。

## 测试覆盖
- [x] 新修复的每个雷都有回归或结构性检查覆盖。
- [x] 并发改动有并发用例。
- [x] 边界场景（空队列 / 满队列 / 关闭中提交）有覆盖。

## 文档
- [x] CHANGELOG.md 追加本轮变更。
- [x] pitfalls.md 追加本轮新踩的雷（现象 / 根因 / 规则 / 状态）。
- [x] ai-pitfalls.md / requirements.md / README.md 同步更新。
