cmake_minimum_required(VERSION 3.20)

project(trieste
    VERSION 0.1.0
    DESCRIPTION "In-memory, thread-safe autocomplete and fuzzy search engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimised build: the whole project is judged on latency, so a
# stray Debug build would produce meaningless benchmark numbers.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TRIESTE_BUILD_TESTS      "Build the unit tests"     ON)
option(TRIESTE_BUILD_BENCHMARKS "Build the benchmarks"     ON)
option(TRIESTE_BUILD_CLI        "Build the demo CLI"       ON)

# ---- Library ----------------------------------------------------------------

add_library(trieste
    src/trie.cpp
    src/autocomplete_engine.cpp
    src/ngram_model.cpp)
add_library(trieste::trieste ALIAS trieste)

target_include_directories(trieste PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(trieste PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(trieste PRIVATE /W4 /permissive-)
else()
    target_compile_options(trieste PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- Demo CLI ---------------------------------------------------------------

if(TRIESTE_BUILD_CLI)
    add_executable(trieste_cli apps/main.cpp)
    target_link_libraries(trieste_cli PRIVATE trieste::trieste)
endif()

# ---- Tests and benchmarks ---------------------------------------------------
#
# Every third-party dependency is fetched by CMake into the build tree. Nothing
# is installed system-wide and nothing outside build/ is written, so build/ is
# disposable and stays out of git.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(TRIESTE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TRIESTE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
