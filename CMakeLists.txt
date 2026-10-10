cmake_minimum_required(VERSION 3.20)

project(PayGuard
    VERSION 1.0.0
    DESCRIPTION "Real-time payment authorization and fraud detection engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PAYGUARD_BUILD_TESTS "Build the GoogleTest suite" ON)
option(PAYGUARD_BUILD_BENCHMARKS "Build the Google Benchmark suite" ON)
option(PAYGUARD_BUILD_TOOLS "Build the data generator and evaluator" ON)
option(PAYGUARD_COVERAGE "Instrument for gcov/lcov coverage (GCC/Clang)" OFF)
set(PAYGUARD_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread, undefined (GCC/Clang)")

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(nlohmann_json)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Compiler settings shared by every target in the project
# ---------------------------------------------------------------------------
add_library(payguard_options INTERFACE)
if(MSVC)
    target_compile_options(payguard_options INTERFACE /W4 /permissive- /utf-8 /EHsc)
    target_compile_definitions(payguard_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(payguard_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(PAYGUARD_SANITIZER AND NOT MSVC)
    target_compile_options(payguard_options INTERFACE
        -fsanitize=${PAYGUARD_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(payguard_options INTERFACE -fsanitize=${PAYGUARD_SANITIZER})
endif()

if(PAYGUARD_COVERAGE AND NOT MSVC)
    target_compile_options(payguard_options INTERFACE --coverage -O0 -g)
    target_link_options(payguard_options INTERFACE --coverage)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(payguard_core STATIC
    src/Money.cpp
    src/StringUtil.cpp
    src/CardProfile.cpp
    src/Account.cpp
    src/AccountStore.cpp
    src/CityTable.cpp
    src/Loaders.cpp
    src/TransactionSource.cpp
    src/Config.cpp
    src/rules/VelocityRule.cpp
    src/rules/AmountAnomalyRule.cpp
    src/rules/GeoImpossibilityRule.cpp
    src/rules/BlocklistRule.cpp
    src/rules/RoundAmountBurstRule.cpp
    src/rules/RuleFactory.cpp
    src/AuthorizationEngine.cpp
    src/AuditLogger.cpp
    src/AlertQueue.cpp
    src/AlertListeners.cpp
    src/RiskyCardTracker.cpp
    src/MetricsCollector.cpp
    src/Pipeline.cpp
    src/Report.cpp
)
target_include_directories(payguard_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(payguard_core
    PUBLIC nlohmann_json::nlohmann_json Threads::Threads payguard_options)

add_executable(payguard src/main.cpp)
target_link_libraries(payguard PRIVATE payguard_core)

if(PAYGUARD_BUILD_TOOLS)
    add_executable(generate_data tools/generate_data.cpp)
    target_link_libraries(generate_data PRIVATE payguard_core)

    add_executable(evaluate tools/evaluate.cpp)
    target_link_libraries(evaluate PRIVATE payguard_core)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PAYGUARD_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(payguard_tests
        tests/test_util.cpp
        tests/test_account.cpp
        tests/test_rules.cpp
        tests/test_engine.cpp
        tests/test_concurrency.cpp
    )
    target_link_libraries(payguard_tests PRIVATE payguard_core GTest::gtest_main)
    target_compile_definitions(payguard_tests PRIVATE
        PAYGUARD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

    include(GoogleTest)
    gtest_discover_tests(payguard_tests DISCOVERY_TIMEOUT 60)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(PAYGUARD_BUILD_BENCHMARKS)
    FetchContent_Declare(benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(bench_engine benchmarks/bench_engine.cpp)
    target_link_libraries(bench_engine PRIVATE payguard_core benchmark::benchmark)
endif()
