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
option(TRIESTE_BUILD_TOOLS      "Build the corpus generator" ON)

# ---- ThreadSanitizer ---------------------------------------------------------
#
# The deterministic race detector that a stress test can never be. Stress tests
# hope an unlucky interleaving shows up; TSan reasons about happens-before edges
# and reports an unsynchronised pair of accesses even when the timing happened
# to work out. That is the difference between "we did not observe a race" and
# "there is no race on the paths we exercised".
#
# GCC or Clang only. MSVC has no ThreadSanitizer, and MinGW's runtime does not
# support it either, so this configuration is Linux-only and runs in CI rather
# than on a Windows development machine.
option(TRIESTE_SANITIZE_THREAD "Build with ThreadSanitizer (GCC/Clang, Linux)" OFF)

if(TRIESTE_SANITIZE_THREAD)
    # WIN32 is checked separately and first: MinGW's g++ reports its compiler id
    # as "GNU" exactly like Linux GCC, so a compiler-id test alone would happily
    # accept a toolchain whose runtime has no ThreadSanitizer at all, and the
    # build would fail much later with a confusing link error.
    if(WIN32)
        message(FATAL_ERROR
            "TRIESTE_SANITIZE_THREAD is not supported on Windows.\n"
            "ThreadSanitizer does not exist for MSVC, and MinGW/MSYS2 g++ "
            "reports itself as GNU but ships no TSan runtime. Run the 'tsan' "
            "preset on Linux -- that is what the CI job does.")
    endif()

    if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        message(FATAL_ERROR
            "TRIESTE_SANITIZE_THREAD requires GCC or Clang, but found "
            "'${CMAKE_CXX_COMPILER_ID}'.")
    endif()

    # Set globally, BEFORE any target is declared, so the FetchContent'd
    # GoogleTest is instrumented too. Uninstrumented code is a blind spot: TSan
    # cannot see accesses it did not instrument, so mixing the two makes it
    # under-report silently rather than fail loudly.
    #
    # -O1 with frame pointers keeps stacks readable. At -O2 the inlining makes a
    # report far harder to trace back to the offending line, and the whole point
    # here is to be handed an exact location.
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)

    message(STATUS "ThreadSanitizer: ENABLED (race detection build)")
endif()

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

# ---- Tools -------------------------------------------------------------------
#
# corpus_gen regenerates data/benchmark_corpus.txt from the upstream word lists.
# The corpus itself is committed, so this is only needed to audit or rebuild it.
if(TRIESTE_BUILD_TOOLS)
    add_executable(trieste_corpus_gen tools/corpus_gen.cpp)
    target_compile_features(trieste_corpus_gen PRIVATE cxx_std_20)
endif()
