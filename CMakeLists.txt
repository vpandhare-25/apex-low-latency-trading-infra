cmake_minimum_required(VERSION 3.20)
project(apex LANGUAGES CXX)

# C++20, no compiler-specific extensions.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build so benchmarks are meaningful.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(APEX_SANITIZE "Build with address + undefined-behavior sanitizers" OFF)

# ---- Core library: everything except main() and tests ----
find_package(ZLIB REQUIRED)  # reads gzip-compressed ITCH files

add_library(apex_core
  src/order_book.cpp
  src/itch_file_reader.cpp
  src/book_builder.cpp
  src/venue.cpp
  src/market_simulator.cpp
  src/router.cpp
  src/analytics.cpp
  src/order_manager.cpp
)
target_include_directories(apex_core PUBLIC include)
target_link_libraries(apex_core PUBLIC ZLIB::ZLIB)
target_compile_options(apex_core PRIVATE -Wall -Wextra -Wpedantic)

if(APEX_SANITIZE)
  target_compile_options(apex_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(apex_core PUBLIC -fsanitize=address,undefined)
endif()

# ---- Command-line tools ----
add_executable(apex_replay tools/apex_replay.cpp)
target_link_libraries(apex_replay PRIVATE apex_core)
add_executable(apex_venues tools/apex_venues.cpp)
target_link_libraries(apex_venues PRIVATE apex_core)
add_executable(apex_extract tools/apex_extract.cpp)
target_link_libraries(apex_extract PRIVATE apex_core)
add_executable(apex_route tools/apex_route.cpp)
target_link_libraries(apex_route PRIVATE apex_core)

# ---- Unit tests (GoogleTest is downloaded automatically) ----
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

# ---- Benchmarks (Google Benchmark is downloaded automatically) ----
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(benchmark)
add_executable(apex_bench bench/apex_bench.cpp)
target_include_directories(apex_bench PRIVATE bench)
target_link_libraries(apex_bench PRIVATE apex_core benchmark::benchmark)
add_executable(apex_latency bench/apex_latency.cpp)
target_include_directories(apex_latency PRIVATE bench)
target_link_libraries(apex_latency PRIVATE apex_core)
add_executable(apex_depth bench/apex_depth.cpp)
target_include_directories(apex_depth PRIVATE bench)
target_link_libraries(apex_depth PRIVATE apex_core)

enable_testing()
add_executable(apex_tests
  tests/test_order_book.cpp
  tests/test_itch.cpp
  tests/test_market_simulator.cpp
  tests/test_router.cpp
  tests/test_order_manager.cpp
  tests/test_order_map.cpp
)
target_link_libraries(apex_tests PRIVATE apex_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(apex_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
