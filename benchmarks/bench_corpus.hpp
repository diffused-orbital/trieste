#pragma once

// Shared fixtures for the M5 benchmark suite: the corpus, the query sets, and
// the latency-percentile helper.
//
// Everything here is deterministic. Benchmarks whose inputs vary between runs
// cannot be compared against yesterday's numbers, which defeats the purpose of
// having them.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "trieste/autocomplete_engine.hpp"

namespace bench {

// ---------------------------------------------------------------------------
// Deterministic pseudo-randomness
// ---------------------------------------------------------------------------

class Lcg {
public:
    explicit Lcg(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33;
    }
    std::size_t below(std::size_t bound) { return static_cast<std::size_t>(next() % bound); }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// The corpus
// ---------------------------------------------------------------------------

struct Entry {
    std::string term;
    int weight;
};

/// Parsed corpus, loaded once. The raw term list is what the naive
/// full-dictionary baseline scans, so it has to exist independently of the
/// engine's internal trie.
inline const std::vector<Entry>& corpus() {
    static const std::vector<Entry>* entries = [] {
        auto* built = new std::vector<Entry>();
        const std::string path = std::string(TRIESTE_DATA_DIR) + "/benchmark_corpus.txt";
        std::ifstream in(path);
        if (!in) {
            throw std::runtime_error("benchmark corpus not found: " + path);
        }
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line.front() == '#') {
                continue;
            }
            const auto split = line.find_last_of(' ');
            if (split == std::string::npos) {
                continue;
            }
            built->push_back(Entry{line.substr(0, split), std::stoi(line.substr(split + 1))});
        }
        return built;
    }();
    return *entries;
}

/// One engine, loaded once and shared by every read benchmark, so corpus
/// construction is never charged to a query measurement.
inline const trieste::AutocompleteEngine& engine() {
    static const trieste::AutocompleteEngine* built = [] {
        auto* e = new trieste::AutocompleteEngine();
        e->loadCorpus(std::string(TRIESTE_DATA_DIR) + "/benchmark_corpus.txt");
        return e;
    }();
    return *built;
}

/// Single words only, for building queries that behave like real user input.
inline const std::vector<std::string>& singleWords() {
    static const std::vector<std::string>* words = [] {
        auto* built = new std::vector<std::string>();
        for (const auto& e : corpus()) {
            if (e.term.find(' ') == std::string::npos) {
                built->push_back(e.term);
            }
        }
        return built;
    }();
    return *words;
}

// ---------------------------------------------------------------------------
// Query sets
// ---------------------------------------------------------------------------

/// Prefixes of the requested length, taken from real corpus words. Sampling
/// from the corpus rather than generating strings matters: a prefix that exists
/// lands in a populated subtree, which is what a real keystroke does. Invented
/// prefixes would mostly miss and measure the empty-result path instead.
inline const std::vector<std::string>& prefixes(std::size_t length) {
    static std::vector<std::vector<std::string>> cache(24);
    auto& slot = cache.at(length);
    if (slot.empty()) {
        Lcg rng(0xBEEF0000ULL + length);
        const auto& words = singleWords();
        for (int i = 0; i < 1000; ++i) {
            const std::string& w = words[rng.below(words.size())];
            if (w.size() >= length) {
                slot.push_back(w.substr(0, length));
            }
        }
    }
    return slot;
}

/// Real words with `edits` character substitutions applied: what a typo looks
/// like. Substitution keeps the length stable so the edit budget is the only
/// variable under test.
inline const std::vector<std::string>& typoQueries(int edits) {
    static std::vector<std::vector<std::string>> cache(4);
    auto& slot = cache.at(static_cast<std::size_t>(edits));
    if (slot.empty()) {
        Lcg rng(0xD00D0000ULL + static_cast<std::uint64_t>(edits));
        const auto& words = singleWords();
        while (slot.size() < 500) {
            std::string w = words[rng.below(words.size())];
            if (w.size() < 6) {  // long enough that the short-query guard admits it
                continue;
            }
            for (int e = 0; e < edits; ++e) {
                w[rng.below(w.size())] = static_cast<char>('a' + rng.below(26));
            }
            slot.push_back(std::move(w));
        }
    }
    return slot;
}

/// Word-boundary queries -- a phrase head plus a trailing space -- which is
/// what makes the engine take the n-gram branch.
inline const std::vector<std::string>& boundaryQueries() {
    static const std::vector<std::string>* queries = [] {
        auto* built = new std::vector<std::string>();
        for (const auto& e : corpus()) {
            const auto space = e.term.find(' ');
            if (space != std::string::npos) {
                built->push_back(e.term.substr(0, space) + " ");
            }
            if (built->size() >= 1000) {
                break;
            }
        }
        return built;
    }();
    return *queries;
}

// ---------------------------------------------------------------------------
// Latency percentiles
// ---------------------------------------------------------------------------

/// Per-call latency samples.
///
/// Google Benchmark's own --benchmark_repetitions statistics compute
/// percentiles ACROSS REPETITIONS, i.e. percentiles of per-repetition means.
/// That is not the query tail: averaging inside each repetition hides exactly
/// the slow calls a p99 is supposed to expose. So each call is timed
/// individually here and the percentiles are taken over the raw samples.
///
/// The two clock reads cost roughly 40ns per sample. Every path measured takes
/// at least ~1us, so the inflation is under ~4% and is not corrected for --
/// stated rather than silently adjusted away.
class LatencyRecorder {
public:
    void reserve(std::size_t n) { samples_.reserve(n); }

    template <typename Fn>
    void time(Fn&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto stop = std::chrono::steady_clock::now();
        samples_.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }

    /// Publish as Google Benchmark counters so the values flow into --benchmark_out
    /// CSV alongside everything else.
    void publish(benchmark::State& state) {
        if (samples_.empty()) {
            return;
        }
        std::sort(samples_.begin(), samples_.end());
        state.counters["p50_us"] = quantile(0.50) / 1000.0;
        state.counters["p95_us"] = quantile(0.95) / 1000.0;
        state.counters["p99_us"] = quantile(0.99) / 1000.0;
        state.counters["max_us"] = static_cast<double>(samples_.back()) / 1000.0;
        state.counters["samples"] = static_cast<double>(samples_.size());
    }

    [[nodiscard]] std::size_t size() const { return samples_.size(); }

private:
    [[nodiscard]] double quantile(double q) const {
        const auto idx = static_cast<std::size_t>(q * static_cast<double>(samples_.size() - 1));
        return static_cast<double>(samples_[idx]);
    }

    std::vector<std::int64_t> samples_;
};

/// Touch every shared fixture before timing starts, so no benchmark pays for
/// lazy construction or first-touch page faults on the trie.
inline void warmUp() {
    static const bool done = [] {
        const auto& e = engine();
        for (int i = 0; i < 2000; ++i) {
            benchmark::DoNotOptimize(e.getSuggestions(prefixes(3)[i % 1000], 5, 0));
        }
        for (int i = 0; i < 200; ++i) {
            benchmark::DoNotOptimize(e.getSuggestions(typoQueries(1)[i % 500], 5, 1));
            benchmark::DoNotOptimize(e.getSuggestions(boundaryQueries()[i % 1000], 5, 0));
        }
        return true;
    }();
    (void)done;
}

}  // namespace bench
