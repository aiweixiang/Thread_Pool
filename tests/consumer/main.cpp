#include "thread_pool.hpp"

int main() {
    mylib::ThreadPool pool(2);
    return pool.submit([] { return 42; }).get() == 42 ? 0 : 1;
}
