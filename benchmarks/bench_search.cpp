// Benchmark harness for trieste.
//
// M1 wires the harness up and establishes a baseline for the two operations
// that exist: exact prefix Top-K, and dynamic insertion. The full suite from
// the spec -- latency percentiles, multi-threaded QPS, and naive O(M*N)
// Levenshtein versus trie-pruned fuzzy search -- lands in M5, marked TODO below.
//
// Run:  ./trieste_bench
//       ./trieste_bench --benchmark_filter=PrefixTopK

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>
#include <vector>

#include "trieste/autocomplete_engine.hpp"

namespace {

/// Deterministic pseudo-random words. A fixed LCG rather than std::mt19937 with
/// a random seed, because a benchmark that measures a different corpus on every
/// run cannot be compared against yesterday's numbers.
class WordGenerator {
public:
    explicit WordGenerator(std::uint64_t seed) : state_(seed) {}

    std::string next() {
        const std::size_t length = 3 + (nextValue() % 10);  // 3..12 characters
        std::string word;
        word.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            word.push_back(static_cast<char>('a' + (nextValue() % 26)));
        }
        return word;
    }

    int nextWeight() { return static_cast<int>(1 + (nextValue() % 10000)); }

private:
    std::uint64_t nextValue() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33;
    }
    std::uint64_t state_;
};

/// One shared dictionary for every read benchmark, built lazily on first use so
/// the corpus construction cost is not attributed to any single benchmark.
const trieste::AutocompleteEngine& dictionary() {
    static const trieste::AutocompleteEngine* engine = [] {
        auto* built = new trieste::AutocompleteEngine();
        WordGenerator generator(0xC0FFEEULL);
        for (int i = 0; i < 100'000; ++i) {  // the spec's 100k-word target
            built->insertQuery(generator.next(), generator.nextWeight());
        }
        return built;
    }();
    return *engine;
}

/// A spread of prefixes so the benchmark measures an average over the trie
/// rather than one lucky branch.
const std::vector<std::string>& probePrefixes() {
    static const std::vector<std::string>* prefixes = [] {
        auto* built = new std::vector<std::string>();
        WordGenerator generator(0xBEEFULL);
        for (int i = 0; i < 512; ++i) {
            built->push_back(generator.next().substr(0, 3));
        }
        return built;
    }();
    return *prefixes;
}

void BM_PrefixTopK(benchmark::State& state) {
    const auto& engine = dictionary();
    const auto& prefixes = probePrefixes();
    const int k = static_cast<int>(state.range(0));

    std::size_t index = 0;
    for (auto _ : state) {
        // Edit budget pinned to zero on purpose. These are three-character
        // probes, so a prefix carries only a handful of the 100k terms and any
        // k above about 5 would fall short and silently divert into the fuzzy
        // fallback -- leaving this benchmark measuring M2's path under M1's
        // name. BM_FuzzyFallback below measures correction deliberately.
        auto results = engine.getSuggestions(prefixes[index++ % prefixes.size()], k,
                                             /*maxEditDistance=*/0);
        benchmark::DoNotOptimize(results);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PrefixTopK)->Arg(1)->Arg(5)->Arg(20);

/// Six-character probes. At that length an exact prefix hit against a random
/// dictionary is unlikely, so almost every call falls through to the fuzzy
/// path -- which is the point: this measures the fallback, not the fast path.
const std::vector<std::string>& missPrefixes() {
    static const std::vector<std::string>* prefixes = [] {
        auto* built = new std::vector<std::string>();
        WordGenerator generator(0xFACEULL);
        for (int i = 0; i < 512; ++i) {
            built->push_back(generator.next().substr(0, 6));
        }
        return built;
    }();
    return *prefixes;
}

void BM_FuzzyFallback(benchmark::State& state) {
    const auto& engine = dictionary();
    const auto& prefixes = missPrefixes();
    const int budget = static_cast<int>(state.range(0));

    std::size_t index = 0;
    for (auto _ : state) {
        auto results = engine.getSuggestions(prefixes[index++ % prefixes.size()], 5, budget);
        benchmark::DoNotOptimize(results);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FuzzyFallback)->Arg(1)->Arg(2);

void BM_InsertQuery(benchmark::State& state) {
    trieste::AutocompleteEngine engine;
    WordGenerator generator(0x1234ULL);
    for (auto _ : state) {
        state.PauseTiming();
        const std::string word = generator.next();
        state.ResumeTiming();
        engine.insertQuery(word, 1);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InsertQuery);

// TODO(M5): latency percentiles -- ->ComputeStatistics("p95", ...) and "p99",
//           asserting the spec's p95 < 2ms / p99 < 5ms budget.
// TODO(M5): multi-threaded QPS -- ->ThreadRange(1, 16) over BM_PrefixTopK once
//           M4's shared_mutex makes concurrent reads safe.
// TODO(M5): naive full-dictionary O(M*N) Levenshtein versus the M2 trie-pruned
//           walk, as a head-to-head on the same corpus.

}  // namespace

BENCHMARK_MAIN();
