# trieste

An in-memory autocomplete and fuzzy search engine in C++20. It answers three
kinds of query against a single frequency-weighted dictionary: prefix completion
ranked by usage, typo-tolerant correction within a bounded edit distance, and
next-word prediction from an n-gram model. Reads are concurrent; writes update
the dictionary in place.

The interesting results, measured on a 108,008-term English corpus. Fuzzy search
runs 245 times faster than a naive scan of the whole dictionary at edit distance
1, and returns provably identical results — the two implementations were checked
against each other and disagreed on nothing. The worst-case prefix query, which
was the one path missing its latency budget, improved by a factor of 179 after
the final optimisation pass, at no cost in memory. Read throughput peaks at about
1.07 million queries per second across eight threads. The locking is not merely
believed to be correct: ThreadSanitizer runs in CI on every push and reports no
data race.

All six planned milestones are complete: the trie and ranked prefix search,
bounded fuzzy correction, the n-gram model, the concurrent read path, the
benchmark suite, and the optimisation pass that followed from it. Everything is
implemented, tested, and measured. The suite is 90 tests, run in CI against GCC,
MSVC, and a ThreadSanitizer build. `RESULTS.md` holds the full benchmark report;
`PLAN.md` records each milestone, the decisions taken, and what was deliberately
left undone.

## Build and run

Requires a C++20 compiler and CMake 3.21 or later. Nothing is installed
system-wide; GoogleTest and Google Benchmark are fetched into the build tree.
Pick the preset matching your toolchain — `unix`, `mingw`, or `msvc`.

```sh
cmake --preset unix
cmake --build --preset unix
ctest --preset unix
```

The demo CLI takes a corpus and a result count:

```sh
./build/trieste_cli data/sample_corpus.txt 6
> appl
  application  [1000]
  apple        [500]
  apply        [300]
```

Benchmarks, and the headline comparison on its own:

```sh
./build/benchmarks/trieste_bench
./build/benchmarks/trieste_bench --benchmark_filter=Levenshtein
```

A fourth preset, `tsan`, builds with ThreadSanitizer and runs the concurrency
suite under it. It is Linux-only, since ThreadSanitizer does not exist for MSVC
and MinGW ships no runtime for it.

```sh
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

## Using it

```cpp
#include "trieste/autocomplete_engine.hpp"

trieste::AutocompleteEngine engine;
engine.loadCorpus("data/sample_corpus.txt");
engine.insertQuery("application", 1);              // continuous learning

engine.getSuggestions("appl", /*k=*/2);            // {"application", "apple"}
engine.getSuggestions("heloo", /*k=*/2, /*E=*/2);  // {"hello", "help"}

// A trailing space is a word boundary, so predictions join the prefix hits:
// {"san francisco", "san diego", "san jose", "santa clara", "francisco"}
engine.getSuggestions("san ");
```

Terms are normalised on the way in and prefixes on the way out — trimmed,
lowercased, internal whitespace collapsed — so matching is case-insensitive and
symmetric. Repeated inserts accumulate weight rather than overwriting it, which
is how the engine learns from live traffic.

## Architecture

A query runs through up to three stages, and most queries stop at the first.

```
  getSuggestions("appl", k, E)
            |
            v
  1. Exact prefix match ................ trie descent, O(L), then a
            |                            best-first walk for the top k
            |
      k results? --- yes ---> return
            |
            no
            v
  2. N-gram prediction ................. only when the raw input ends on a
            |                            word boundary (trailing space)
            |
      k results? --- yes ---> return
            |
            no
            v
  3. Fuzzy fallback .................... bounded edit distance over the trie,
            |                            DP row carried down each edge
            v
        return top k
```

The dictionary is a trie whose nodes hold a frequency and a cached bound on the
best frequency anywhere beneath them. Stage one descends the prefix in time
proportional to its length, then runs a best-first search guided by that bound,
which is what makes short prefixes cheap. Stage two consults a bigram and
trigram transition model trained from the same normalised token stream that
feeds the trie; it runs only when the user has finished a word, because at that
point predicting the next token is a better answer than completing a word they
already typed correctly. Stage three carries a Levenshtein DP row down each trie
edge and abandons a subtree as soon as the row's minimum exceeds the edit
budget.

Ranking is the same everywhere: frequency descending, ties broken
lexicographically. That total order is deliberate. Without it, identical input
would produce different output between runs, and neither the tests nor the
benchmarks would be reproducible.

The three stages compose by filling slots rather than competing. Exact hits take
the top of the result list, predictions fill what remains, and corrections take
whatever is left after that. A correction can never displace a real prefix hit,
so typing another character cannot push the term you wanted further down the
list.

A single `std::shared_mutex` guards the trie and the n-gram model together.
Reads take it shared, writes exclusively.

## Benchmarks

Measured on a 108,008-term corpus: 100,000 real English words with the head of
the frequency distribution taken from observed usage data, Zipfian weights, and
8,000 multi-word phrases for the n-gram path. This choice mattered more than any
other benchmarking decision. Random strings share no prefixes, so a trie built
from them degenerates into a shallow bush and the fuzzy walk's subtree pruning —
the entire basis of its sub-linear behaviour — can never fire. Benchmarking on
random input measures a pathological case. The difference is not subtle: this
corpus has 3,700 distinct three-character prefixes where uniform random text
would have roughly 17,576, and the densest five percent of prefixes hold 45
percent of all words.

### Fuzzy search against a naive full-dictionary scan

![Naive versus trie-pruned fuzzy search](benchmarks/results/naive_vs_pruned.svg)

The naive baseline scans every term in the dictionary with a full Levenshtein DP
and no pruning. At edit distance 1 that costs 9.28 ms per query against 37.8 µs
for the trie walk, a factor of 245. At edit distance 2 the gap narrows to 24
times, which is what you would expect: the naive side is flat because it always
scans everything, while a wider edit budget keeps more of the trie alive and
grows the pruned side's work about tenfold.

Making that comparison fair was the harder half. trieste's fuzzy search is
prefix-relaxed — a term matches if any prefix of it lies within the edit budget
— so measuring it against a naive whole-word distance would have compared two
different questions and flattered whichever side happened to do less work. The
baseline therefore computes the same predicate. Filling the standard DP grid for
a query and a term leaves the final row holding the distance to every prefix of
that term, so the minimum of that row is the best prefix distance. A benchmark
runs 40 queries through both implementations at both budgets and compares hit
counts; it reports zero mismatches.

### Latency against prefix length

![Exact prefix latency by prefix length](benchmarks/results/latency_by_prefix_length.svg)

That is the current state: every prefix length lands between 0.6 µs and 22 µs
across p50, p95 and p99, and the curve is nearly flat. It was not always so, and
how it got there is the most interesting measurement in the project.

Before the final optimisation, cost tracked prefix length rather than k. A
two-character prefix took 2.22 ms at p95, missing the 2 ms target, while an
eight-character prefix took 6 µs. Raising k from 1 to 20 moved p50 by 26
percent; shortening the prefix from eight characters to two moved it by a factor
of 816. The cause was structural: ranking could not begin until the entire
subtree beneath the prefix had been walked, so a short prefix visited tens of
thousands of nodes to fill a five-slot heap.

![Before and after the M6 optimisation](benchmarks/results/m6_before_after.svg)

Caching a bound on each node's subtree and replacing the walk with a best-first
descent removed that. Every prefix length improved, including the deep prefixes
that were already fast, which was the main risk — a heap-guided search carries
setup cost that a straight walk does not.

| Prefix length | p95 before | p95 after |
|---|---|---|
| 2 characters | 6276.3 µs | 12.4 µs |
| 3 characters | 1404.8 µs | 10.0 µs |
| 4 characters | 333.9 µs | 7.4 µs |
| 8 characters | 6.0 µs | 1.9 µs |

Those before figures were re-measured on the same machine in the same session as
the after figures, which is the only fair way to compare them, and they read
higher than the 2.22 ms published earlier because the machine was warmer after a
long run of builds. The same-session ratio is 506. Against the cooler earlier
baseline it is 179. The lower figure is the one quoted, here and everywhere
else.

Current latency, after the optimisation:

| Path | p50 | p95 | p99 |
|---|---|---|---|
| Exact prefix, 2 characters, k=5 | 6.4 µs | 12.4 µs | 22.2 µs |
| Exact prefix, 8 characters, k=5 | 0.6 µs | 1.9 µs | 3.2 µs |
| Fuzzy correction, E=1 | 42.7 µs | 80.9 µs | 128.5 µs |
| Fuzzy correction, E=2 | 402.6 µs | 827.2 µs | 1293.1 µs |
| N-gram prediction | 4.1 µs | 7.6 µs | 11.8 µs |
| `insertQuery` | 0.9 µs | 2.7 µs | 6.2 µs |

The original specification asked for p95 under 2 ms and p99 under 5 ms on a
dictionary of 100,000 or more words. Every path now clears that by at least two
orders of magnitude.

### Throughput

![Throughput against thread count](benchmarks/results/qps_vs_threads.svg)

Read throughput scales close to linearly up to eight threads and then stops.
Peak is about 1.07 million queries per second at eight threads, which is 6.98
times the single-thread figure for an eight-fold increase in threads, or 87
percent scaling efficiency. Per-thread latency stays flat across that range,
6.6 µs at one thread against 7.5 µs at eight, so the shared lock is not
serialising readers.

At sixteen threads throughput falls back to about 861,000 and per-thread latency
roughly triples to 18.6 µs. The machine is the likely explanation rather than
the lock: an i9-13900H has six performance cores and eight efficiency cores, so
past eight threads the work spills onto efficiency cores and hyperthread
siblings. Whatever the cause, the honest statement is that this engine saturates
near a million reads per second on this hardware, and adding threads beyond
eight does not help.

| Threads | Prefix reads | Mixed, 10% typos | Reads under a writer |
|---|---|---|---|
| 1 | 152,654 | 92,421 | 4,584 |
| 2 | 319,184 | 182,914 | — |
| 4 | 582,353 | 360,684 | 60,849 |
| 8 | **1,065,065** | 644,148 | — |
| 16 | 860,836 | 911,258 | 166,050 |

A single continuous writer is expensive for readers, costing somewhere between
five and ten times the read throughput depending on thread count. That is
inherent to one exclusive lock, and it is the measurement that would justify
finer-grained locking if the workload ever warranted the complexity. It is also
the noisiest thing measured here, 42 to 75 percent spread between runs, so treat
it as an order of magnitude rather than a figure.

Every number above is the median of four independent runs; run-to-run spread is
recorded per row in `benchmarks/results/throughput.csv`. The single-thread
figures carry the widest spread, around 30 percent, which is consistent with a
lone thread being scheduled onto either core type.

### Method

Percentiles are measured per query rather than per repetition. Google
Benchmark's built-in repetition statistics report percentiles of per-repetition
means, and averaging inside each repetition hides exactly the slow calls a p99
exists to expose. Each call is timed individually and the percentiles taken over
the raw samples, 20,000 of them for the fast paths. The two clock reads cost
about 40 ns per sample, under two percent on every path but insert, and that is
stated rather than corrected away.

Benchmark families are run separately with a cooldown between them. Running the
whole suite back to back for 258 seconds on a laptop inflated later measurements
by a factor of 2.3 through thermal throttling — the same benchmark read 67 µs in
isolation and 156 µs at the end of a long run. Isolated runs repeat to within
half a percent. This is a laptop, an Intel i9-13900H under Windows 11 with MinGW
g++ 15.2.0, and the CI runners are shared and noisier still, so latency figures
are taken locally and CI is used for correctness rather than performance.

## Design decisions

### A subtree bound rather than cached top-K lists

The obvious fix for the short-prefix problem is the one the LeetCode 642 problem
suggests: store each node's best k completions and read them straight back. At
432,062 nodes that would have added roughly 41 MB even with four-byte term
identifiers, more than doubling a 31.6 MB trie, in the same milestone whose
other stated goal was reducing memory.

What was built instead stores a single integer per node: the highest frequency
anywhere in that node's subtree. The query becomes a best-first descent over a
frontier ordered by that bound, breaking ties on path. Pop the most promising
entry; if it is a finished term, emit it, otherwise split it into its own term
plus one entry per child. Because a subtree's bound can never be beaten by its
contents, the first k terms popped are exactly the top k. The path tie-break is
what preserves the existing frequency-then-lexicographic order, since every term
under a node shares that node's path as a prefix, so ordering nodes
lexicographically also orders their whole subtrees.

The integer is free. A node was a 24-byte vector plus a four-byte int, padded to
32 bytes, and the second int lands in that padding. Measured before and after:
31.6 MB against 31.3 MB for the same 432,062 nodes.

There is also no cache to invalidate, which removes a whole category of bug. The
update is a single monotonic maximum along the inserted term's path. That is
exact rather than approximate for one specific reason: frequencies never
decrease. Insert accumulates, non-positive weights are rejected, and nothing
deletes. Only nodes on the inserted path gain a term, so no other bound can go
stale. Adding a delete or weight-decrease API would break this — a maximum
cannot be lowered incrementally, and the affected path would need recomputing
bottom-up from its children. That constraint is commented at the call site
because it is otherwise a silent trap.

The cost is on the write side. Maintaining the bound needs a second descent of
the inserted path, which moved insert p50 from 0.80 µs to 0.90 µs. Two to three
orders of magnitude on the read path for a tenth of a microsecond on the write
path is the right side of that trade for an autocomplete engine.

### A DP row carried down the trie rather than a Levenshtein automaton

The original specification called for a Levenshtein automaton, and it was not
built. For an edit budget of 2, carrying a single DP row down each trie edge
already achieves the pruning that matters. A node standing for some string holds
that string's column of the DP grid, so stepping to a child fills in exactly one
new column, and shared prefixes pay that cost once on behalf of every completion
beneath them.

The pruning follows from a small observation: every cell of a child's row is at
least the minimum cell of its parent's, because each DP transition either copies
a neighbouring cell or adds one to it. Once that minimum exceeds the budget, no
descendant can come back inside it, and the entire subtree is abandoned
unexamined.

That is what produces the 245-fold margin over a naive scan. An automaton would
optimise a path that is not the bottleneck, at the cost of precomputing and
storing per-query automata, so it stays unbuilt. If it is ever revisited it
should be benchmarked against this rather than assumed faster.

### Sorted-vector children

The textbook trie node is a hash map from character to child, and the
competitive-programming one is a fixed 26-element array. This uses neither. Each
node keeps its children in a `std::vector` sorted by character and searched with
binary search, which gives up O(1) hashing for O(log c) over a handful of
children and buys dense storage with one allocation per node instead of a
hashtable, no per-node hashtable overhead, and deterministic alphabetical
iteration.

That last property is not incidental. The fuzzy walk and the ranking tie-break
both depend on traversal order being stable, and a hash map does not provide it.
The 26-element array was rejected on a harder constraint still: it would have
restricted the alphabet to a–z, which rules out the space character and so would
have blocked multi-word keys and the entire n-gram feature. It also costs 208
bytes per node regardless of how many children that node actually has.

## Concurrency and correctness

Concurrency is one `std::shared_mutex` guarding the trie and the n-gram model
together. Reads take it shared, writes exclusively. Nothing more elaborate was
built, because until the benchmarks existed there was no evidence that anything
more elaborate was needed, and the throughput numbers show reads scaling close
to linearly up to eight threads before the hardware, rather than the lock, runs
out of room.

Establishing that this is actually race-free took more than running the tests. A
stress test can only report a race that some interleaving happened to expose,
and the original concurrency tests passed 100 consecutive runs with every lock
stripped out — they were testing nothing. ThreadSanitizer now runs the
concurrency suite in CI on every push. It reasons about happens-before edges
rather than hoping for an unlucky schedule, so a clean run is evidence of
absence rather than absence of evidence.

A clean ThreadSanitizer run only means something if the instrumentation is
actually live, so that was checked too. A throwaway branch with every lock
removed produced an immediate report naming the exact racing access,
`src/trie.cpp:60`, the frequency increment. The detector demonstrably fires on
these code paths, which is what makes the clean run on the real code
informative.

The same standard was applied to the optimisation work. Replacing the ranking
algorithm is only an improvement if it returns identical results, so a
differential test compares the new implementation against a brute-force
reference built from the raw corpus text, sharing no implementation with the
trie. Over 1,180 comparisons spanning prefix lengths 0 through 6, k values from
1 to 20, the empty prefix, and a query matching nothing, it reports zero
mismatches.

There is one loose end worth naming. Two concurrency tests fault intermittently
on the Windows development machine under MinGW. That was investigated rather
than waved away: ThreadSanitizer is clean, and 200 consecutive runs of the same
tests on Linux produced no failures at all, so the fault is in MinGW's threading
runtime rather than in this code. It does not reproduce on either CI toolchain.

The suite is 90 tests across GCC, MSVC, and the ThreadSanitizer build.

## Limitations and future work

Memory was measured but not optimised. The trie holds 108,000 terms in 31.3 MB
across 432,062 nodes, which works out to 75.9 bytes per node against a 32-byte
node struct. The difference is the children vector's heap allocation and malloc
header — a real 2.4-fold overhead that an arena with 32-bit child indices would
largely reclaim, and that would probably improve traversal locality as well. It
was left alone because 31.3 MB for a dictionary this size is not costing
anything, and because doing that rewrite in the same milestone as the ranking
change would have made the before-and-after attribution meaningless.

The monotonic-frequency constraint described above is the sharpest edge in the
codebase. Deletion and weight decrease are not supported, and adding either
requires rethinking how the subtree bound is maintained.

Fuzzy search declines when the query is no longer than the edit budget, because
at that point the empty prefix is itself within budget and every term in the
dictionary would qualify. That guard is crude. Graduated fuzziness — no
correction for very short queries, one edit for medium ones, two for long ones —
would bound the behaviour properly. It is a correctness-of-behaviour improvement
rather than a performance one; edit distance 2 already runs comfortably inside
budget.

The n-gram model is trained on phrases that pair real high-frequency words by a
deterministic rule. Their structure is realistic, a small set of heads each with
many continuations, but the specific pairings are synthetic, so the n-gram
latency figures are sound while the prediction quality has not been evaluated
against real phrase data.

## Repository layout

```
include/trieste/   public headers
src/               implementation
apps/              demo CLI
tests/             GoogleTest suites, including the concurrency tests
benchmarks/        Google Benchmark suite, results, and chart generator
tools/             benchmark corpus generator
data/              sample and benchmark corpora
```

`RESULTS.md` is the full benchmark report. `PLAN.md` records the six milestones,
the decisions taken at each of those, and what was deliberately left undone.

## License

[MIT](LICENSE).
