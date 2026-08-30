// M5: aggregate throughput under concurrent load.
//
// Exercises M4's shared_mutex read path. The question: does the shared lock
// actually let readers run in parallel, or does it serialise them?
//
// This manages its own threads rather than using Google Benchmark's threaded
// mode. GB reports per-iteration time averaged across threads and derives
// items_per_second from it, which is genuinely ambiguous to read as an
// aggregate figure -- and aggregate QPS is exactly the number being claimed.
// Here the harness starts N threads behind a barrier, measures wall-clock from
// barrier release to the last join, and divides total queries by that. There is
// nothing to interpret.
//
// Run:  ./trieste_bench --benchmark_filter=Throughput

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_corpus.hpp"

namespace {

constexpr std::size_t kQueriesPerThread = 4000;

/// Runs `work` on `threads` threads, all released together, and returns the
/// aggregate queries per second measured against wall-clock.
template <typename Work>
double measureQps(int threads, Work&& work) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));

    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            work(t);
        });
    }

    // Thread creation is excluded from the measurement: wait until every thread
    // is parked at the barrier, THEN start the clock and release them. Otherwise
    // spawn cost would be charged to the query rate and would look like poor
    // scaling at high thread counts.
    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }

    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : pool) {
        th.join();
    }
    const auto stop = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(stop - start).count();
    const double total = static_cast<double>(threads) * static_cast<double>(kQueriesPerThread);
    return total / seconds;
}

/// Pure exact-prefix reads: shared_lock only, no writer ever contends.
void BM_Throughput_PrefixReads(benchmark::State& state) {
    bench::warmUp();
    const auto& e = bench::engine();
    const auto& queries = bench::prefixes(3);
    const int threads = static_cast<int>(state.range(0));

    double qps = 0;
    for (auto _ : state) {
        qps = measureQps(threads, [&](int id) {
            // Offset per thread so they are not replaying the same query in
            // lockstep, which would keep one cache line unrealistically hot.
            std::size_t i = static_cast<std::size_t>(id) * 97;
            for (std::size_t n = 0; n < kQueriesPerThread; ++n) {
                benchmark::DoNotOptimize(e.getSuggestions(queries[i++ % queries.size()], 5, 0));
            }
        });
    }
    state.counters["qps"] = qps;
    state.counters["threads"] = threads;
}
BENCHMARK(BM_Throughput_PrefixReads)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->Iterations(1)->Repetitions(3)->UseRealTime();

/// A realistic keystroke stream: ~10% of traffic is a typo needing the fuzzy
/// fallback, the rest are exact prefix hits. Fuzzy is far more expensive, so
/// this shows what a modest typo rate costs in aggregate throughput.
void BM_Throughput_MixedQueries(benchmark::State& state) {
    bench::warmUp();
    const auto& e = bench::engine();
    const auto& exact = bench::prefixes(3);
    const auto& typos = bench::typoQueries(1);
    const int threads = static_cast<int>(state.range(0));

    double qps = 0;
    for (auto _ : state) {
        qps = measureQps(threads, [&](int id) {
            std::size_t i = static_cast<std::size_t>(id) * 97;
            for (std::size_t n = 0; n < kQueriesPerThread; ++n) {
                ++i;
                if (i % 10 == 0) {
                    benchmark::DoNotOptimize(e.getSuggestions(typos[i % typos.size()], 5, 1));
                } else {
                    benchmark::DoNotOptimize(e.getSuggestions(exact[i % exact.size()], 5, 0));
                }
            }
        });
    }
    state.counters["qps"] = qps;
    state.counters["threads"] = threads;
}
BENCHMARK(BM_Throughput_MixedQueries)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->Iterations(1)->Repetitions(3)->UseRealTime();

/// Read throughput while a writer is continuously inserting. Readers take the
/// shared lock, the writer takes it exclusively, so this is where the
/// shared_mutex either holds up or falls over. Compare against PrefixReads at
/// the same thread count to see what a live write stream costs.
void BM_Throughput_ReadsUnderWriter(benchmark::State& state) {
    bench::warmUp();
    const auto& queries = bench::prefixes(3);
    const auto& words = bench::singleWords();
    const int threads = static_cast<int>(state.range(0));

    double qps = 0;
    for (auto _ : state) {
        // A private engine: this one gets mutated, so it must not be the shared
        // read-only fixture every other benchmark depends on.
        trieste::AutocompleteEngine local;
        local.loadCorpus(std::string(TRIESTE_DATA_DIR) + "/benchmark_corpus.txt");

        std::atomic<bool> stop{false};
        std::thread writer([&] {
            std::size_t i = 0;
            while (!stop.load(std::memory_order_acquire)) {
                local.insertQuery(words[i++ % words.size()], 1);
            }
        });

        qps = measureQps(threads, [&](int id) {
            std::size_t i = static_cast<std::size_t>(id) * 97;
            for (std::size_t n = 0; n < kQueriesPerThread; ++n) {
                benchmark::DoNotOptimize(
                    local.getSuggestions(queries[i++ % queries.size()], 5, 0));
            }
        });

        stop.store(true, std::memory_order_release);
        writer.join();
    }
    state.counters["qps"] = qps;
    state.counters["threads"] = threads;
}
BENCHMARK(BM_Throughput_ReadsUnderWriter)
    ->Arg(1)->Arg(4)->Arg(16)
    ->Iterations(1)->Repetitions(3)->UseRealTime();

}  // namespace
