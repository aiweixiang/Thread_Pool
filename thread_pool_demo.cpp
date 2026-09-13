#include "thread_pool.hpp"

#include <iostream>
#include <memory>

int main() {
    mylib::ThreadPool pool(4);

    auto sum = pool.submit([](int a, int b) { return a + b; }, 1, 2);
    std::cout << "1 + 2 = " << sum.get() << '\n';

    auto value = std::make_unique<int>(21);
    auto doubled = pool.submit(
        [](std::unique_ptr<int> input) { return *input * 2; },
        std::move(value));
    std::cout << "21 * 2 = " << doubled.get() << '\n';

    mylib::ThreadPool bounded(2, 8);
    auto optionalFuture = bounded.try_submit([](int n) { return n * n; }, 9);
    if (optionalFuture) {
        std::cout << "9 * 9 = " << optionalFuture->get() << '\n';
    } else {
        std::cout << "try_submit rejected the task\n";
    }

    bounded.shutdown(mylib::ShutdownMode::Drain);
    pool.shutdown(mylib::ShutdownMode::Drain);
    return 0;
}
