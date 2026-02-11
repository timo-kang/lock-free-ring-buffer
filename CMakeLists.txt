cmake_minimum_required(VERSION 3.20)

project(lock_free_ring_buf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LF_RING_BUILD_TESTS "Build unit tests" ON)
option(LF_RING_BUILD_BENCHMARKS "Build benchmarks" ON)

include(CTest)

add_library(lf_ring
  src/shared_ring_buffer.cpp
  src/shared_ring_buffer_mpmc.cpp
  src/shared_ring_buffer_spsc.cpp
  src/shared_memory_mapping.cpp
)

target_include_directories(lf_ring PUBLIC include)

target_compile_features(lf_ring PUBLIC cxx_std_20)

if (MSVC)
  target_compile_options(lf_ring PRIVATE /W4)
else()
  target_compile_options(lf_ring PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)

target_link_libraries(lf_ring PUBLIC Threads::Threads)

if (LF_RING_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(lf_ring_tests
    tests/test_ring_buffer.cpp
    tests/test_typed_message.cpp
  )
  target_link_libraries(lf_ring_tests PRIVATE lf_ring gtest_main)

  include(GoogleTest)
  gtest_discover_tests(lf_ring_tests)
endif()

if (LF_RING_BUILD_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(lf_ring_bench
    bench/benchmark_ring_buffer.cpp
  )
  target_link_libraries(lf_ring_bench PRIVATE lf_ring benchmark::benchmark)
endif()
