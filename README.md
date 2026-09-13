# ThreadPool

> 一个单头文件、C++17、没有第三方依赖的线程池。

[![CI](https://github.com/aiweixiang/Thread_Pool/actions/workflows/ci.yml/badge.svg)](https://github.com/aiweixiang/Thread_Pool/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen)

把任务丢进去，拿 `std::future` 收结果。需要背压就用有界队列，不想等就用
`try_submit()`。没有第三方依赖，也没有隐藏的全局状态。

## 为什么值得一看

- **一个头文件**：复制 `thread_pool.hpp` 就能用，CMake 只是可选项。
- **现代接口**：支持 lambda、函数对象、成员函数指针、move-only 参数和返回值。
- **异常不掉线**：任务异常通过 `future.get()` 原样透传。
- **两种提交方式**：`submit()` 提供背压，`try_submit()` 永不阻塞等待队列空间。
- **两种关闭模式**：`Drain` 跑完排队任务，`Discard` 立即丢掉尚未开始的任务。
- **可观测**：能看到排队数、执行中数量和 worker 数量，而不是靠猜。

它适合中小型项目里的独立任务并行、批处理、轻量后台工作。它不是任务依赖图、
工作窃取或无锁调度器；这些复杂机制没有偷偷塞进来。

## 30 秒上手

```cpp
#include "thread_pool.hpp"

#include <future>
#include <iostream>

int main() {
    mylib::ThreadPool pool(4);

    std::future<int> answer = pool.submit(
        [](int a, int b) { return a + b; }, 20, 22);

    std::cout << answer.get() << '\n';  // 42
}
```

`ThreadPool` 析构时默认执行 `Drain` 模式的 shutdown，已经提交的任务不会被悄悄丢掉。

## API 速览

| API | 作用 |
|---|---|
| `submit(f, args...)` | 提交任务并返回 `std::future<R>`；有界队列满时阻塞 |
| `try_submit(f, args...)` | 立即返回 `std::optional<std::future<R>>`，队列满或已关闭时为空 |
| `wait()` | 等待调用时刻之前已成功提交的任务完成，线程池继续可用 |
| `shutdown(mode)` | 关闭池；`Drain` 执行完排队任务，`Discard` 丢弃未开始任务 |
| `pendingTasks()` | 查看仍在队列中的任务数 |
| `activeTasks()` | 查看正在执行的任务数 |
| `threadCount()` | 查看 worker 槽位数 |
| `isShutdown()` | 判断关闭是否已经开始，不代表 worker 已 join |

### 有界队列与背压

```cpp
mylib::ThreadPool pool(2, /*maxQueueSize=*/64);

auto future = pool.submit(work);              // 队列满时等待空位
auto optional = pool.try_submit(work);        // 永不等待空位

if (!optional) {
    // 可能是队列已满，也可能是池已关闭；可用 isShutdown() 辅助区分。
}
```

`maxQueueSize == 0` 表示无界队列，适合突发量可控的场景。长期生产者快于消费者时，
无界队列会持续占用内存；生产环境应评估背压，优先使用有界队列。

### 关闭

```cpp
pool.shutdown(mylib::ShutdownMode::Drain);    // 等待排队和活动任务结束
pool.shutdown(mylib::ShutdownMode::Discard);  // 只等待活动任务，丢弃排队任务
```

`shutdown()` 和析构没有超时或强制取消。活动任务如果永久不结束，它们也会一直等待。

## 使用边界

线程池很好用，也最容易被误用。下面几条值得先记住：

- 不要在一个任务里同步等待同一线程池中的其他任务；worker 被占满后会死锁。
- `submit()` / `try_submit()` 会在池锁内构造任务，用户类型的拷贝/移动构造不得回调本池。
- 销毁池之前，先 join 所有可能调用 `submit()`、`try_submit()`、`wait()` 或
  `shutdown()` 的外部线程。
- `std::ref` 不延长对象生命周期，被引用对象必须存活到任务结束。
- 不调用 `future.get()` 就无法获得任务异常；`wait()` 只等待，不重抛。

完整规则和原因见 [`docs/pitfalls.md`](docs/pitfalls.md)。它是项目唯一的风险事实源。

## 构建与验证

需要 CMake 3.14+ 和支持 C++17 的编译器。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizer 构建：

```bash
cmake -S . -B build-tsan -DTHREAD_POOL_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure

cmake -S . -B build-asan -DTHREAD_POOL_ENABLE_ASAN=ON -DTHREAD_POOL_ENABLE_UBSAN=ON
cmake --build build-asan --parallel
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

CI 会运行普通测试、TSan 和 ASan+UBSan。

## 安装

```bash
cmake --install build --prefix /your/prefix
```

安装后头文件位于 include 根目录，可以继续使用：

```cpp
#include "thread_pool.hpp"
```

## 文档地图

| 文件 | 内容 |
|---|---|
| `AGENTS.md` | 修改代码前必须遵守的简短规则 |
| `docs/requirements.md` | 功能范围、验收标准和明确不做的事情 |
| `docs/pitfalls.md` | 已知风险、状态、原因与不变量，唯一风险事实源 |
| `docs/review-checklist.md` | 提交前的可执行检查清单 |
| `CHANGELOG.md` | 面向用户的版本变化 |

## License

[MIT](LICENSE) © 2026 Ai Weixiang
