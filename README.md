# trieste

An in-memory, thread-safe **autocomplete and fuzzy search engine** in C++20.

Ingests a frequency-weighted corpus and serves per-keystroke suggestions in
O(L) on the prefix length, ranked Top-K by usage frequency. The roadmap adds
bounded typo tolerance (edit distance ≤ 2) over the trie, an n-gram context
model for multi-word input, and a concurrent read path — each stage measured by
a Google Benchmark suite rather than assumed.

> **Status: Milestone 1.** The trie, corpus loading, exact prefix search and
> Top-K ranking are implemented and tested. Fuzzy search, n-grams and
> concurrency are stubbed behind the public API and marked `TODO(Mx)`. See
> [PLAN.md](PLAN.md) for the full roadmap.

## Quick start

Requires a C++20 compiler and CMake ≥ 3.21 (≥ 3.20 if you configure without
presets). Nothing is installed system-wide — GoogleTest and Google Benchmark
are fetched by CMake into `build/`, which is disposable and git-ignored.

Pick the preset matching your toolchain: `unix` for Linux/macOS, `mingw` for
MinGW-w64 / MSYS2 g++ on Windows, `msvc` for Visual Studio.

```sh
cmake --preset unix
cmake --build --preset unix
ctest --preset unix
```

`cmake --list-presets` shows them all. Presets exist because on Windows without
Visual Studio, a bare `cmake -S . -B build` falls back to the `NMake Makefiles`
generator and fails with `CMAKE_CXX_COMPILER not set` — the `mingw` preset pins
the Ninja generator and g++ so that cannot happen.

Then try the demo CLI:

```sh
./build/trieste_cli data/sample_corpus.txt
> appl
  application  [1000]
  apple        [500]
  apply        [300]
```

And the benchmarks:

```sh
./build/trieste_bench
./build/trieste_bench --benchmark_filter=PrefixTopK
```

Binaries land in `build/`, or `build/Release/` under the multi-config `msvc`
preset. Substitute your preset name in the commands above; the `mingw` preset
needs `ninja` on PATH (`winget install Ninja-build.Ninja`, or
`pacman -S mingw-w64-ucrt-x86_64-ninja` inside MSYS2).

## Usage

```cpp
#include "trieste/autocomplete_engine.hpp"

trieste::AutocompleteEngine engine;
engine.loadCorpus("data/sample_corpus.txt");   // "term <whitespace> weight"
engine.insertQuery("application", 1);          // continuous learning

auto results = engine.getSuggestions("appl", /*k=*/2, /*maxEditDistance=*/1);
// -> {"application", "apple"}
```

`getScoredSuggestions` returns the same ranking with the frequency that earned
each slot. Terms are normalised on the way in and prefixes on the way out —
trimmed, lowercased, internal whitespace collapsed — so matching is
case-insensitive and symmetric.

### Corpus format

One entry per line. A trailing all-digit token is the weight; without one the
weight defaults to 1. The split happens on the *last* whitespace, so multi-word
terms stay intact. Blank lines and `#` comments are skipped.

```
application 1000
apple 500
san francisco 900
new york
```

Repeated entries **accumulate** rather than overwrite — that is how the engine
learns from live traffic.

## How it works

**Trie with sorted-vector children.** Each node stores its children in a
`std::vector` sorted by character and searched with binary search. Against the
textbook `unordered_map` node this trades O(1) hashing for O(log c) over a
handful of children, and buys dense cache-friendly storage, no per-node
hashtable overhead, and deterministic alphabetical traversal — which the fuzzy
walk and the tie-break rule both depend on.

**Bounded min-heap for Top-K.** Prefix lookup descends L nodes, then walks the
subtree offering every terminal node to a heap capped at k. The heap is ordered
so its *weakest* survivor sits on top, making eviction O(1) to find: scanning n
candidates costs O(n log k) time and O(k) space instead of sorting all n.
Ranking is frequency-descending with a lexicographic tie-break, so identical
input always produces identical output.

## Layout

```
include/trieste/   public headers — trie.hpp, autocomplete_engine.hpp
src/               implementation
apps/              demo CLI
tests/             GoogleTest suites
benchmarks/        Google Benchmark harness
data/              sample corpus
```

## Roadmap

| | Milestone | Status |
|---|---|---|
| M1 | Scaffold, Trie, exact prefix, Top-K min-heap | ✅ done |
| M2 | Fuzzy search — bounded edit distance (E ≤ 2) over the trie | planned |
| M3 | Bigram/trigram Markov model for next-token prediction | planned |
| M4 | Concurrency — `std::shared_mutex` read/write path | planned |
| M5 | Benchmark suite — p50/p95/p99, multi-threaded QPS, naive vs. pruned | planned |
| M6 | Memory and latency optimisation, driven by M5's numbers | planned |

Details and design decisions in [PLAN.md](PLAN.md).

## License

[MIT](LICENSE).
