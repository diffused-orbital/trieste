FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE)

# Make GoogleTest link the shared CRT so it matches the rest of the build on
# MSVC. Harmless everywhere else.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

add_executable(trieste_tests
    trie_test.cpp
    engine_test.cpp
    fuzzy_test.cpp
    ngram_test.cpp
    concurrency_test.cpp)

# Threads is needed for std::thread / std::shared_mutex on Linux (pthreads).
# On MSVC and MinGW-w64 this is a no-op because the threading primitives are
# already part of the default link set.
find_package(Threads REQUIRED)
target_link_libraries(trieste_tests PRIVATE trieste::trieste GTest::gtest_main Threads::Threads)

# Lets the tests find the shipped sample corpus without depending on the working
# directory ctest happens to run them from.
target_compile_definitions(trieste_tests PRIVATE
    TRIESTE_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

if(MSVC)
    target_compile_options(trieste_tests PRIVATE /W4)
else()
    target_compile_options(trieste_tests PRIVATE -Wall -Wextra)
endif()

include(GoogleTest)
gtest_discover_tests(trieste_tests)
