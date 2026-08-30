// M5: query latency on a realistic corpus.
//
// Every benchmark here reports p50/p95/p99 as counters rather than leaning on
// the mean. For an interactive engine the mean is close to meaningless: the
// user notices the slow keystroke, not the average one.
//
// Corpus: 100,000 real English words with Zipfian weights plus 8,000 phrases.
// See tools/corpus_gen.cpp for how it is built and RESULTS.md for why random
// strings would have made these numbers a fiction.
//
// Run:  ./trieste_bench --benchmark_filter=Prefix
//       ./trieste_bench --benchmark_out=results.csv --benchmark_out_format=csv

#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_corpus.hpp"

namespace {

// ---------------------------------------------------------------------------
// 1. Exact prefix Top-K
// ---------------------------------------------------------------------------

/// Latency against k, at a fixed prefix length. The edit budget is pinned to
/// zero so this measures the exact path alone -- otherwise a prefix with fewer
/// than k matches silently diverts into the fuzzy fallback and the benchmark
/// would be reporting M2's cost under M1's name.
void BM_PrefixTopK_ByK(benchmark::State& state) {
    bench::warmUp();
    const auto& e = bench::engine();
    const auto& queries = bench::prefixes(3);
    const int k = static_cast<int>(state.range(0));

    bench::LatencyRecorder rec;
    rec.reserve(static_cast<std::size_t>(state.max_iterations));

    std::size_t i = 0;
    for (auto _ : state) {
        const std::string& q = queries[i++ % queries.size()];
        rec.time([&] { benchmark::DoNotOptimize(e.getSuggestions(q, k, 0)); });
    }
    rec.publish(state);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PrefixTopK_ByK)->Arg(1)->Arg(5)->Arg(10)->Arg(20)->Iterations(20000);

/// Latency against prefix length at fixed k. Short prefixes are the expensive
/// case: the subtree beneath them holds a large share of the dictionary, and
/// M1's Top-K walk has to visit all of it.
void BM_PrefixTopK_ByLength(benchmark::State& state) {
    bench::warmUp();
    const auto& e = bench::engine();
    const auto len = static_cast<std::size_t>(state.range(0));
    const auto& queries = bench::prefixes(len);

    bench::LatencyRecorder rec;
    rec.reserve(static_cast<std::size_t>(state.max_iterations));

    std::size_t i = 0;
    for (auto _ : state) {
        const std::string& q = queries[i++ % queries.size()];
        rec.time([&] { benchmark::DoNotOptimize(e.getSuggestions(q, 5, 0)); });
    }
    rec.publish(state);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PrefixTopK_ByLength)->Arg(2)->Arg(3)->Arg(4)->Arg(6)->Arg(8)->Iterations(20000);

// ---------------------------------------------------------------------------
// 2. Fuzzy fallback
// ---------------------------------------------------------------------------

/// Typo correction at E=1 and E=2. Queries are real words with that many
/// characters substituted, so each one has a genuine correction to find rather
/// than being a miss that returns early.
void BM_FuzzyFallback(benchmark::State& state) {
    bench::warmUp();
    const auto& e = bench::engine();
    const int budget = static_cast<int>(state.range(0));
    const auto& queries = bench::typoQueries(budget);

    bench::LatencyRecorder rec;
    rec.reserve(static_cast<std::size_t>(state.max_iterations));

    std::size_t i = 0;
    for (auto _ : state) {
        const std::string& q = queries[i++ % queries.size()];
        // k=20 keeps the exact pass from ever satisfying k on its own, so the
        // fuzzy path is genuinely entered on every iteration.
        rec.time([&] { benchmark::DoNotOptimize(e.getSuggestions(q, 20, budget)); });
    }
    rec.publish(state);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FuzzyFallback)->Arg(1)->Iterations(4000);
BENCHMARK(BM_FuzzyFallback)->Arg(2)->Iterations(1000);

// ---------------------------------------------------------------------------
// 3. N-gram next-token prediction
// ---------------------------------------------------------------------------

/// Word-boundary queries. The trailing space is what routes the query into the
/// n-gram branch; without it this would just be another prefix benchmark.
void BM_NgramPrediction(benchmark::State& state) {
    bench::warmUp();
    const auto& e = bench::engine();
    const auto& queries = bench::boundaryQueries();

    bench::LatencyRecorder rec;
    rec.reserve(static_cast<std::size_t>(state.max_iterations));

    std::size_t i = 0;
    for (auto _ : state) {
        const std::string& q = queries[i++ % queries.size()];
        rec.time([&] { benchmark::DoNotOptimize(e.getSuggestions(q, 5, 0)); });
    }
    rec.publish(state);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NgramPrediction)->Iterations(20000);

// ---------------------------------------------------------------------------
// 4. Write path
// ---------------------------------------------------------------------------

/// insertQuery, for reference: it takes the exclusive lock and updates both the
/// trie and the n-gram model, so it bounds how fast the engine can learn.
void BM_InsertQuery(benchmark::State& state) {
    bench::warmUp();
    trieste::AutocompleteEngine local;
    const auto& words = bench::singleWords();

    bench::LatencyRecorder rec;
    rec.reserve(static_cast<std::size_t>(state.max_iterations));

    std::size_t i = 0;
    for (auto _ : state) {
        const std::string& w = words[i++ % words.size()];
        rec.time([&] { local.insertQuery(w, 1); });
    }
    rec.publish(state);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InsertQuery)->Iterations(50000);

}  // namespace
