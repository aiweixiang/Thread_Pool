cmake_minimum_required(VERSION 3.14)

project(ThreadPool LANGUAGES CXX)

include(GNUInstallDirs)

find_package(Threads REQUIRED)

add_library(thread_pool INTERFACE)
target_include_directories(
    thread_pool
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(thread_pool INTERFACE cxx_std_17)
target_link_libraries(thread_pool INTERFACE Threads::Threads)

option(THREAD_POOL_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(THREAD_POOL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(THREAD_POOL_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(THREAD_POOL_ENABLE_TSAN AND (THREAD_POOL_ENABLE_ASAN OR THREAD_POOL_ENABLE_UBSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan or UBSan in one build")
endif()

function(thread_pool_enable_sanitizer target)
    if(THREAD_POOL_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
    if(THREAD_POOL_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
    if(THREAD_POOL_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
endfunction()

function(thread_pool_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_executable(thread_pool_demo thread_pool_demo.cpp)
target_link_libraries(thread_pool_demo PRIVATE thread_pool)
thread_pool_enable_warnings(thread_pool_demo)
thread_pool_enable_sanitizer(thread_pool_demo)

include(CTest)
if(BUILD_TESTING)
    add_executable(thread_pool_tests test_thread_pool.cpp)
    target_link_libraries(thread_pool_tests PRIVATE thread_pool)
    thread_pool_enable_warnings(thread_pool_tests)
    thread_pool_enable_sanitizer(thread_pool_tests)

    add_test(NAME thread_pool_tests COMMAND thread_pool_tests)

    set(THREAD_POOL_TEST_TIMEOUT 30)
    if(THREAD_POOL_ENABLE_TSAN OR THREAD_POOL_ENABLE_ASAN OR THREAD_POOL_ENABLE_UBSAN)
        set(THREAD_POOL_TEST_TIMEOUT 120)
    endif()
    set_tests_properties(thread_pool_tests PROPERTIES TIMEOUT ${THREAD_POOL_TEST_TIMEOUT})
endif()

install(FILES thread_pool.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS thread_pool EXPORT ThreadPoolTargets)
install(
    EXPORT ThreadPoolTargets
    FILE ThreadPoolTargets.cmake
    NAMESPACE mylib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThreadPool)
