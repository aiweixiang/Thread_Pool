cmake_minimum_required(VERSION 3.14)

project(ThreadPool VERSION 0.1.2 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

find_package(Threads REQUIRED)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(THREAD_POOL_IS_TOP_LEVEL ON)
else()
    set(THREAD_POOL_IS_TOP_LEVEL OFF)
endif()

option(THREAD_POOL_BUILD_TESTS "Build ThreadPool tests" ${THREAD_POOL_IS_TOP_LEVEL})
option(THREAD_POOL_BUILD_DEMO "Build ThreadPool demo" ${THREAD_POOL_IS_TOP_LEVEL})
option(THREAD_POOL_WARNINGS_AS_ERRORS "Treat project warnings as errors" ${THREAD_POOL_IS_TOP_LEVEL})
option(THREAD_POOL_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(THREAD_POOL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(THREAD_POOL_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(THREAD_POOL_ENABLE_TSAN AND (THREAD_POOL_ENABLE_ASAN OR THREAD_POOL_ENABLE_UBSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan or UBSan in one build")
endif()

add_library(thread_pool INTERFACE)
add_library(mylib::thread_pool ALIAS thread_pool)

target_include_directories(
    thread_pool
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(thread_pool INTERFACE cxx_std_17)
target_link_libraries(thread_pool INTERFACE Threads::Threads)

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
        if(THREAD_POOL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wshadow
                -Wconversion
                -Wsign-conversion)
        if(THREAD_POOL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(THREAD_POOL_BUILD_DEMO)
    add_executable(thread_pool_demo thread_pool_demo.cpp)
    target_link_libraries(thread_pool_demo PRIVATE mylib::thread_pool)
    thread_pool_enable_warnings(thread_pool_demo)
    thread_pool_enable_sanitizer(thread_pool_demo)
endif()

if(THREAD_POOL_BUILD_TESTS)
    include(CTest)

    if(BUILD_TESTING)
        add_executable(thread_pool_tests test_thread_pool.cpp)
        target_link_libraries(thread_pool_tests PRIVATE mylib::thread_pool)
        thread_pool_enable_warnings(thread_pool_tests)
        thread_pool_enable_sanitizer(thread_pool_tests)

        add_executable(
            thread_pool_robustness_tests
            test_thread_pool_robustness.cpp)
        target_link_libraries(
            thread_pool_robustness_tests
            PRIVATE mylib::thread_pool)
        thread_pool_enable_warnings(thread_pool_robustness_tests)
        thread_pool_enable_sanitizer(thread_pool_robustness_tests)

        add_test(NAME thread_pool_tests COMMAND thread_pool_tests)
        add_test(
            NAME thread_pool_robustness_tests
            COMMAND thread_pool_robustness_tests)

        if(TARGET thread_pool_demo)
            add_test(NAME thread_pool_demo COMMAND thread_pool_demo)
        endif()

        set(THREAD_POOL_TEST_TIMEOUT 30)
        if(THREAD_POOL_ENABLE_TSAN OR THREAD_POOL_ENABLE_ASAN OR THREAD_POOL_ENABLE_UBSAN)
            set(THREAD_POOL_TEST_TIMEOUT 120)
        endif()
        set_tests_properties(
            thread_pool_tests
            thread_pool_robustness_tests
            PROPERTIES TIMEOUT ${THREAD_POOL_TEST_TIMEOUT})
        if(TARGET thread_pool_demo)
            set_tests_properties(thread_pool_demo PROPERTIES TIMEOUT ${THREAD_POOL_TEST_TIMEOUT})
        endif()
    endif()
endif()

set(THREAD_POOL_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/ThreadPool")

configure_package_config_file(
    cmake/ThreadPoolConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolConfig.cmake"
    INSTALL_DESTINATION ${THREAD_POOL_INSTALL_CMAKEDIR})
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES thread_pool.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS thread_pool EXPORT ThreadPoolTargets)
install(
    EXPORT ThreadPoolTargets
    FILE ThreadPoolTargets.cmake
    NAMESPACE mylib::
    DESTINATION ${THREAD_POOL_INSTALL_CMAKEDIR})
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolConfigVersion.cmake"
    DESTINATION ${THREAD_POOL_INSTALL_CMAKEDIR})
