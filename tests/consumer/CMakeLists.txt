cmake_minimum_required(VERSION 3.14)

project(ThreadPoolConsumer LANGUAGES CXX)

find_package(ThreadPool 0.1.1 CONFIG REQUIRED)

add_executable(thread_pool_consumer main.cpp)
target_link_libraries(thread_pool_consumer PRIVATE mylib::thread_pool)
