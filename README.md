# ThreadPool

C++17 header-only 线程池，`#include "thread_pool.hpp"` 即可使用。

## 开发前必读（按顺序）

1. docs/ai-pitfalls.md —— 规则集，改代码前后各读一遍
2. docs/review-checklist.md —— 交付前逐条勾选
3. docs/requirements.md —— 需求与验收标准

想知道某条规则的来龙去脉，查 docs/pitfalls.md。

## 文件

    thread_pool.hpp         核心实现
    thread_pool_demo.cpp    使用示例
    test_thread_pool.cpp    单元测试
    CMakeLists.txt          构建脚本
    CHANGELOG.md            版本历史
    LICENSE                 MIT License
    docs/                   开发文档（需求 / 规则 / 雷点 / 自检清单）
    .github/workflows/       GitHub Actions CI

## 快速上手

    mylib::ThreadPool pool(4);
    auto f = pool.submit([](int a, int b) { return a + b; }, 1, 2);
    std::cout << f.get() << "\n";   // 3

有界队列（maxQueueSize > 0 时，队列满则 submit 阻塞；不想阻塞用 try_submit）：

    mylib::ThreadPool pool(2, /*maxQueueSize=*/10);
    pool.submit(task);                 // 队列满时阻塞
    auto opt = pool.try_submit(task);  // 队列满时返回 std::nullopt

关闭：

    pool.shutdown(mylib::ShutdownMode::Drain);    // 等排队任务跑完（析构默认）
    pool.shutdown(mylib::ShutdownMode::Discard);  // 丢弃排队任务

## 构建 / 测试

    cmake -S . -B build && cmake --build build
    ctest --test-dir build --output-on-failure

Sanitizer：

    cmake -S . -B build-tsan -DTHREAD_POOL_ENABLE_TSAN=ON
    cmake --build build-tsan && ctest --test-dir build-tsan

安装：

    cmake --install build --prefix <prefix>

安装后 `thread_pool.hpp` 位于 include 根目录，可继续直接使用
`#include "thread_pool.hpp"`。CI 会运行普通测试、TSan、ASan 和 UBSan。

## License

本项目采用 [MIT License](LICENSE)。
