FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.4
    GIT_SHALLOW    TRUE)

set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
# Google Benchmark compiles itself with -Werror by default, which turns any new
# compiler's warnings into a build failure in somebody else's code.
set(BENCHMARK_ENABLE_WERROR      OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(benchmark)

add_executable(trieste_bench
    bench_search.cpp
    bench_levenshtein.cpp
    bench_throughput.cpp)

target_link_libraries(trieste_bench PRIVATE trieste::trieste benchmark::benchmark_main)

# Lets the benchmarks find the committed corpus without depending on the
# working directory they happen to be launched from.
target_compile_definitions(trieste_bench PRIVATE
    TRIESTE_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

if(MSVC)
    target_compile_options(trieste_bench PRIVATE /W4)
else()
    target_compile_options(trieste_bench PRIVATE -Wall -Wextra)
endif()
