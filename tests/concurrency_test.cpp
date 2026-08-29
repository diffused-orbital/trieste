// M4 — Concurrency tests for AutocompleteEngine
//
// These tests verify that the shared_mutex read/write path added in M4
// is data-race-free under concurrent load.  Each test spins up a small
// thread pool, hammers the engine, and then asserts the post-condition
// that the single-threaded code guarantees.
//
// What the tests cover:
//   ConcurrentReads          — many threads reading simultaneously never crash
//                              and always return a consistent, non-empty result.
//   ConcurrentWrites         — many threads inserting simultaneously do not lose
//                              writes: the final frequency is the sum of all
//                              per-thread contributions.
//   ConcurrentReadWriteMix   — readers and writers running together; after all
//                              writes complete every reader must have seen at
//                              least the initial corpus (no partial-write tear).
//   LoadCorpusConcurrent     — loadCorpus from multiple threads merges cleanly
//                              and does not deadlock (it holds the lock only
//                              during the batch insert, not during the I/O).
//
// Run under ThreadSanitizer (TSAN) to get definitive data-race detection:
//   cmake --preset unix -DCMAKE_CXX_FLAGS="-fsanitize=thread"
//   cmake --build --preset unix && ctest --preset unix

#include "trieste/autocomplete_engine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using trieste::AutocompleteEngine;

// ---------------------------------------------------------------------------
// Helper: write a small corpus to a temp file and remove it on destruction.
// ---------------------------------------------------------------------------
class TempCorpus {
public:
    explicit TempCorpus(const std::string& contents) {
        path_ = std::filesystem::temp_directory_path() /
                ("trieste_conc_" + std::to_string(++counter_) + ".txt");
        std::ofstream out(path_);
        out << contents;
    }
    ~TempCorpus() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempCorpus(const TempCorpus&) = delete;
    TempCorpus& operator=(const TempCorpus&) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// ---------------------------------------------------------------------------
// Helper: run `fn` on `nThreads` threads concurrently, join all before return.
// ---------------------------------------------------------------------------
template <typename Fn>
void runConcurrently(int nThreads, Fn&& fn) {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(nThreads));
    for (int i = 0; i < nThreads; ++i) {
        threads.emplace_back(fn, i);
    }
    for (auto& t : threads) {
        t.join();
    }
}

// ---------------------------------------------------------------------------
// Test 1: Concurrent reads — readers never see a crash or empty result.
//
// The engine is loaded once before the threads start (all writes are done),
// so this exercises the shared-lock read path only.  Under TSAN a missing
// std::shared_lock on getSuggestions would be flagged here.
// ---------------------------------------------------------------------------
TEST(Concurrency, ConcurrentReads) {
    AutocompleteEngine engine;
    engine.insertQuery("application", 1000);
    engine.insertQuery("apple",       500);
    engine.insertQuery("apply",       300);
    engine.insertQuery("apricot",     150);

    constexpr int kReaders = 16;
    constexpr int kItersPerThread = 200;
    std::atomic<int> failCount{0};

    runConcurrently(kReaders, [&](int /*id*/) {
        for (int i = 0; i < kItersPerThread; ++i) {
            auto results = engine.getSuggestions("appl", 3);
            if (results.empty()) {
                ++failCount;
            }
            // The top result must always be "application" — it has the highest
            // weight and the corpus is not mutated while readers are running.
            if (results[0] != "application") {
                ++failCount;
            }
        }
    });

    EXPECT_EQ(failCount.load(), 0);
}

// ---------------------------------------------------------------------------
// Test 2: Concurrent writes — all insertQuery calls must be accounted for.
//
// N threads each insert "ping" with weight 1 exactly M times.  After all
// threads join the frequency stored in the engine must equal N*M.
// ---------------------------------------------------------------------------
TEST(Concurrency, ConcurrentWrites) {
    AutocompleteEngine engine;

    // Seed the term up front so every thread below only bumps an ALREADY
    // EXISTING node's frequency. An unsynchronised run then fails as a clean
    // lost-update assertion rather than a structural crash, which is both
    // deterministic and far easier to diagnose.
    engine.insertQuery("ping", 1);

    // The workload has to be large enough, and start closely enough together,
    // that an unlocked `frequency += weight` actually drops increments. The
    // original 8x100 finished before the threads meaningfully overlapped and
    // passed even with every lock removed, so it proved nothing.
    constexpr int kWriters  = 8;
    constexpr int kIters    = 20000;
    constexpr int kExpected = 1 + kWriters * kIters;

    std::atomic<int> ready{0};

    runConcurrently(kWriters, [&](int /*id*/) {
        // Spin barrier: no thread starts writing until all of them have
        // arrived, so the increments genuinely contend instead of running
        // back-to-back as the threads are still being spawned.
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < kWriters) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kIters; ++i) {
            engine.insertQuery("ping", 1);
        }
    });

    const auto scored = engine.getScoredSuggestions("ping", 1);
    ASSERT_EQ(scored.size(), 1u);
    // Exact equality is the whole point: a single lost increment fails here.
    EXPECT_EQ(scored[0].frequency, kExpected);
}

// ---------------------------------------------------------------------------
// Test 3: Mixed read / write — readers and writers run simultaneously.
//
// Writers insert "zap" in a loop; readers query for "za" and verify they
// always get a *consistent* result (never a partially-constructed vector).
// After all threads finish the engine must have seen every write.
// ---------------------------------------------------------------------------
TEST(Concurrency, ConcurrentReadWriteMix) {
    AutocompleteEngine engine;
    // Seed one term so readers never see an empty corpus from the start.
    engine.insertQuery("zap", 1);

    constexpr int kReaders  = 6;
    constexpr int kWriters  = 4;
    constexpr int kIters    = 4000;
    constexpr int kThreads  = kReaders + kWriters;

    std::atomic<int> readErrors{0};
    // Shared spin barrier across readers AND writers, so reads genuinely
    // overlap writes rather than trailing behind thread spawn.
    std::atomic<int> ready{0};
    const auto waitForAll = [&ready] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < kThreads) {
            std::this_thread::yield();
        }
    };

    // Launch readers.
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            waitForAll();
            for (int j = 0; j < kIters; ++j) {
                // Must not crash; result may vary depending on write progress.
                auto r = engine.getSuggestions("za", 5);
                if (r.empty()) {
                    // "zap" was seeded before threads started, so the list
                    // must always contain at least one entry.
                    ++readErrors;
                }
            }
        });
    }

    // Launch writers.
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([&](int id) {
            waitForAll();
            for (int j = 0; j < kIters; ++j) {
                engine.insertQuery("zap", 1);
                // Also insert a unique word per writer thread so we can count writers.
                engine.insertQuery("writer" + std::to_string(id), 1);
            }
        }, i);
    }

    for (auto& t : writers) { t.join(); }
    for (auto& t : readers) { t.join(); }

    EXPECT_EQ(readErrors.load(), 0);

    // Total "zap" frequency = 1 (seed) + kWriters * kIters.
    const auto scored = engine.getScoredSuggestions("zap", 1);
    ASSERT_EQ(scored.size(), 1u);
    EXPECT_EQ(scored[0].frequency, 1 + kWriters * kIters);

    // Each writer must have contributed its unique word.
    EXPECT_EQ(engine.termCount(), static_cast<std::size_t>(1 + kWriters));
}

// ---------------------------------------------------------------------------
// Test 4: loadCorpus is concurrent-safe.
//
// Two threads each load a different corpus file simultaneously.  After both
// join the engine must contain all terms from both files, proving the batch
// write-lock in loadCorpus does not produce lost updates or deadlocks.
// ---------------------------------------------------------------------------
TEST(Concurrency, LoadCorpusConcurrent) {
    const TempCorpus corpusA("mango 400\nmelon 300\nmint 200\n");
    const TempCorpus corpusB("banana 700\nblueberry 600\nblackberry 500\n");

    AutocompleteEngine engine;

    std::thread tA([&] { engine.loadCorpus(corpusA.path()); });
    std::thread tB([&] { engine.loadCorpus(corpusB.path()); });
    tA.join();
    tB.join();

    // All six terms must be present.
    EXPECT_EQ(engine.termCount(), 6u);

    // Each prefix must resolve correctly.
    auto m = engine.getSuggestions("m", 5);
    EXPECT_EQ(m.size(), 3u);  // mango, melon, mint (order by freq)

    auto b = engine.getSuggestions("b", 5);
    EXPECT_EQ(b.size(), 3u);  // banana, blueberry, blackberry

    // Verify correct top pick for each group.
    EXPECT_EQ(m[0], "mango");
    EXPECT_EQ(b[0], "banana");
}

// ---------------------------------------------------------------------------
// Test 5: termCount / nodeCount are safe to call during concurrent writes.
//
// This is a smoke-test: we verify the calls do not crash and that the final
// count is at least as large as the number of unique terms inserted.
// ---------------------------------------------------------------------------
TEST(Concurrency, MetricsUnderConcurrentWrites) {
    AutocompleteEngine engine;

    constexpr int kWriters = 8;
    constexpr int kTerms   = 5;  // each writer inserts a different unique word

    std::atomic<int> crashCount{0};

    runConcurrently(kWriters, [&](int id) {
        engine.insertQuery("term" + std::to_string(id), 1);
        // Reading metrics while others are writing must not crash.
        try {
            (void)engine.termCount();
            (void)engine.nodeCount();
        } catch (...) {
            ++crashCount;
        }
    });

    EXPECT_EQ(crashCount.load(), 0);
    EXPECT_GE(engine.termCount(), static_cast<std::size_t>(kTerms));
}

}  // namespace
