# 交付前自检

> 每次改完逐条勾选，结果写进提交说明：通过 / 不适用 / 需讨论。
> 规则原文见 ai-pitfalls.md，本文件只列可执行检查项。

## 编译与测试
- [ ] 构建无警告（-Wall -Wextra）。
- [ ] ctest 全绿。
- [ ] 并发改动跑过 TSan。
- [ ] 跑过 ASan/UBSan。

## 本轮新雷（对应 R1–R5）
- [ ] R1：三处返回类型推导都用 Func&，一致。
- [ ] R1：可调用对象同时有 operator()& / operator()&& 时返回类型正确。
- [ ] R2：try_submit 失败时入参语义明确（未消费 或 注释声明）。
- [ ] R3：shutdown(Discard) 被丢弃任务在锁外析构。
- [ ] R4：所有 notify 在解锁之后。
- [ ] R5：submit / try_submit 有 [[nodiscard]]。

## 历史雷（不退化）
- [ ] 调用处无无意义的 std::move(func)。
- [ ] 头文件「使用禁忌」仍覆盖：递归 submit、任务内 shutdown/wait/析构。
- [ ] 头文件「已知语义」仍覆盖：broken_promise、并发 shutdown 参数取舍、
      wait 与 shutdown 并发的边界。
- [ ] shutdown(bool) 旧重载保留。
- [ ] 并发 shutdown 语义未变（先到者 mode 生效，call_once join）。
- [ ] 构造函数异常仍 join 已建线程再抛。
- [ ] workerLoop 仍有 try/catch(...)。
- [ ] 锁内无异常逃逸。

## 测试覆盖
- [ ] 新修复的每个雷都有回归用例。
- [ ] 并发改动有并发用例。
- [ ] 边界场景（空队列 / 满队列 / 关闭中提交）有覆盖。

## 文档
- [ ] CHANGELOG.md 追加本轮变更。
- [ ] pitfalls.md 追加本轮新踩的雷（现象 / 根因 / 规则 / 状态）。
