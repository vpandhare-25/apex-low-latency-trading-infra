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
add_library(apex_core
  src/order_book.cpp
)
target_include_directories(apex_core PUBLIC include)
target_compile_options(apex_core PRIVATE -Wall -Wextra -Wpedantic)

if(APEX_SANITIZE)
  target_compile_options(apex_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(apex_core PUBLIC -fsanitize=address,undefined)
endif()

# ---- Unit tests (GoogleTest is downloaded automatically) ----
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(apex_tests
  tests/test_order_book.cpp
)
target_link_libraries(apex_tests PRIVATE apex_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(apex_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
