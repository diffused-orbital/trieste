// M5 headline: naive full-dictionary Levenshtein vs the trie-pruned walk.
//
// The spec asks for this comparison directly, and it is the one number that
// justifies the whole fuzzy design. Both sides answer the SAME question on the
// SAME corpus with the SAME queries; only the algorithm differs.
//
// FAIRNESS -- the part that is easy to get wrong.
//
// trieste's fuzzy search is prefix-relaxed: a term matches when ANY prefix of
// it lies within E edits of the query, and it reports the smallest such prefix
// distance. Comparing that against a naive whole-word edit distance would be
// comparing two different questions, and would flatter whichever side happened
// to do less work.
//
// So the naive baseline computes the same predicate. Filling the standard DP
// grid for (query, term) leaves the final row holding ED(query, term[0..j]) for
// every j, so the minimum of that row IS the best prefix distance. The naive
// side takes the min of the final row -- identical semantics, no shortcuts,
// scanning every term in the dictionary.
//
// Run:  ./trieste_bench --benchmark_filter=Levenshtein

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_corpus.hpp"
#include "trieste/trie.hpp"

namespace {

/// Textbook Levenshtein DP over two rows, returning the smallest distance from
/// `query` to any PREFIX of `term` -- the minimum of the final row.
///
/// No pruning, no early exit, no bound: that is the point. This is the O(|q|*|t|)
/// per term, O(N*|q|*|t|) per query cost that the trie exists to avoid.
int bestPrefixDistance(std::string_view query, std::string_view term,
                       std::vector<int>& previous, std::vector<int>& current) {
    const std::size_t m = query.size();
    previous.assign(m + 1, 0);
    for (std::size_t i = 0; i <= m; ++i) {
        previous[i] = static_cast<int>(i);  // ED(query[0..i), "")
    }

    int best = previous[m];  // the empty prefix is a candidate too
    current.resize(m + 1);

    for (std::size_t j = 1; j <= term.size(); ++j) {
        current[0] = static_cast<int>(j);
        for (std::size_t i = 1; i <= m; ++i) {
            const int substitute = previous[i - 1] + (query[i - 1] == term[j - 1] ? 0 : 1);
            const int insert = previous[i] + 1;
            const int remove = current[i - 1] + 1;
            current[i] = std::min({substitute, insert, remove});
        }
        best = std::min(best, current[m]);
        previous.swap(current);
    }
    return best;
}

/// The naive engine: scan the entire dictionary, score every term, keep the
/// ones inside the budget.
std::size_t naiveFuzzySearch(std::string_view query, int maxDistance,
                             std::vector<int>& bufA, std::vector<int>& bufB) {
    std::size_t hits = 0;
    for (const auto& entry : bench::corpus()) {
        if (bestPrefixDistance(query, entry.term, bufA, bufB) <= maxDistance) {
            ++hits;
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------

void BM_Levenshtein_Naive(benchmark::State& state) {
    bench::warmUp();
    const int budget = static_cast<int>(state.range(0));
    const auto& queries = bench::typoQueries(budget);

    std::vector<int> bufA, bufB;
    bench::LatencyRecorder rec;
    rec.reserve(static_cast<std::size_t>(state.max_iterations));

    std::size_t i = 0;
    for (auto _ : state) {
        const std::string& q = queries[i++ % queries.size()];
        rec.time([&] { benchmark::DoNotOptimize(naiveFuzzySearch(q, budget, bufA, bufB)); });
    }
    rec.publish(state);
    state.SetItemsProcessed(state.iterations());
}
// Deliberately few iterations: each one scans all 108,008 terms.
BENCHMARK(BM_Levenshtein_Naive)->Arg(1)->Iterations(40);
BENCHMARK(BM_Levenshtein_Naive)->Arg(2)->Iterations(40);

void BM_Levenshtein_TriePruned(benchmark::State& state) {
    bench::warmUp();
    const int budget = static_cast<int>(state.range(0));
    const auto& queries = bench::typoQueries(budget);

    // The trie is rebuilt here rather than reusing the shared engine so both
    // sides are measured against exactly the same term set with no engine-level
    // ranking or n-gram work included in the timing.
    static const trieste::Trie* trie = [] {
        auto* t = new trieste::Trie();
        for (const auto& entry : bench::corpus()) {
            t->insert(entry.term, entry.weight);
        }
        return t;
    }();

    bench::LatencyRecorder rec;
    rec.reserve(static_cast<std::size_t>(state.max_iterations));

    std::size_t i = 0;
    for (auto _ : state) {
        const std::string& q = queries[i++ % queries.size()];
        rec.time([&] { benchmark::DoNotOptimize(trie->fuzzySearch(q, budget)); });
    }
    rec.publish(state);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Levenshtein_TriePruned)->Arg(1)->Iterations(4000);
BENCHMARK(BM_Levenshtein_TriePruned)->Arg(2)->Iterations(1000);

// ---------------------------------------------------------------------------
// Equivalence check
// ---------------------------------------------------------------------------

/// A speedup only means something if both sides return the same answer. This
/// runs once as a benchmark so it shows up in the same output as the timings:
/// for a sample of queries, the naive hit count and the trie hit count must
/// agree exactly. A mismatch reports a nonzero counter and invalidates the
/// comparison above.
void BM_Levenshtein_Equivalence(benchmark::State& state) {
    bench::warmUp();
    static const trieste::Trie* trie = [] {
        auto* t = new trieste::Trie();
        for (const auto& entry : bench::corpus()) {
            t->insert(entry.term, entry.weight);
        }
        return t;
    }();

    std::vector<int> bufA, bufB;
    double mismatches = 0;
    double checked = 0;

    for (auto _ : state) {
        for (int budget = 1; budget <= 2; ++budget) {
            const auto& queries = bench::typoQueries(budget);
            for (std::size_t i = 0; i < 20; ++i) {
                const std::string& q = queries[i];
                const std::size_t naive = naiveFuzzySearch(q, budget, bufA, bufB);
                const std::size_t pruned = trie->fuzzySearch(q, budget).size();
                ++checked;
                if (naive != pruned) {
                    ++mismatches;
                }
            }
        }
    }
    state.counters["queries_checked"] = checked;
    state.counters["MISMATCHES"] = mismatches;
}
BENCHMARK(BM_Levenshtein_Equivalence)->Iterations(1);

}  // namespace
