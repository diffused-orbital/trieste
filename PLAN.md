# trieste — milestone roadmap

Working agreement: build in small, verifiable milestones. **After each
milestone everything compiles and every test passes before the next one
starts.** One clean commit per milestone. Correct-and-simple first; optimise
later with benchmarks driving the decision.

The public API (`AutocompleteEngine`) is a **stable facade from day one** —
later-phase methods exist from M1 with their final signatures, stubbed and
marked `// TODO(Mx)`. Milestones fill in behaviour behind the facade; they do
not reshape it.

---

## M1 — Scaffold, repo setup, Trie, exact prefix, Top-K  (done)

- Project layout, CMake building library + tests + demo CLI, FetchContent for
  GoogleTest and Google Benchmark.
- Repo furniture: `git init`, `.gitignore`, `README.md`, `LICENSE`,
  `.github/workflows/ci.yml`.
- Core Trie with frequency-weighted insertion (`insertQuery`) and `loadCorpus`.
- Exact prefix search returning Top-K by frequency via a bounded min-heap.
- Tests including the spec's worked example.
- Benchmark harness wired and running.

**Decisions taken**

- *Trie child representation:* `std::vector<std::pair<char, unique_ptr<Node>>>`
  kept sorted by character, binary-searched. Chosen over `unordered_map`
  (per-node hashtable overhead, cache-hostile, non-deterministic iteration) and
  over a fixed `array<Node*, 26>` (a–z only, which would block M3's multi-word
  keys, and 208 bytes per node regardless of fan-out). The sorted vector gives
  dense storage and deterministic alphabetical DFS, which M2's fuzzy traversal
  and the tie-break rule both rely on.
- *Ranking order:* frequency descending, ties broken lexicographically. A total
  order keeps Top-K output byte-identical across runs, without which neither
  the tests nor the M5 benchmarks would be reproducible.
- *Normalisation:* trim, lowercase ASCII, collapse internal whitespace runs —
  applied symmetrically on insert and on query.

## M2 — Fuzzy search, bounded edit distance (E ≤ 2)  (done)

One Levenshtein DP row carried down each trie edge, with subtree pruning.
Fires only when exact prefix matches < K, so the exact path stays the fast
default.

**Decisions taken**

- *Match semantics: fuzzy-prefix, "correct then complete".* A term matches if
  ANY prefix of it is within E edits of the query, and its reported distance is
  the smallest such prefix distance. Chosen over whole-term ED, which cannot
  complete (`"aple"` would reach `"apple"` and stop, never `"application"`) and
  is wrong for a per-keystroke engine where the user is mid-word. Subsumes
  whole-term matching.
- *The prune.* Every cell of a child's row is ≥ the minimum cell of the parent
  row, because each DP transition either copies a neighbouring cell or adds one
  to it. So once `min(row) > E`, no descendant can bring `row[m]` back inside
  the budget and the whole subtree is abandoned unexamined. This is what makes
  the walk sub-O(N).
- *Short-query guard.* Fuzzy declines when `|query| <= E`. At that point the
  EMPTY prefix is itself within budget, every term qualifies, and the search
  degenerates into a full scan. See the open question below.
- *Merge order: exact block first.* Exact hits keep their slots in frequency
  order; corrections fill only the remainder, sorted by distance ascending then
  frequency descending. A typo-match can never displace a real prefix hit, so
  typing another character never pushes the wanted term down the list.
- *Row storage.* One DP row per depth in a `std::deque`, reused across the
  traversal. A deque rather than a vector because growing it must not
  invalidate row references held by frames further up the recursion.

**Corrections applied to the original spec** (it was AI-generated from a
one-line idea and is a loose brief, not authoritative):

- The Levenshtein Automaton is NOT built. For E ≤ 2 the DP-row walk with
  pruning is already sub-O(N); a precomputed automaton is over-engineering.
  Left as an optional M6 benchmark comparison, if it is worth doing at all.
- "Out-of-order words" is out of scope — unscoped, much harder, and nothing in
  the API or the worked examples supports it.
- The spec's typo example states `maxEditDistance=1`, but `ED("heloo","help")`
  is 2, so `"help"` is unreachable at a budget of 1 under any standard edit
  distance. The example's *output* is right and its *parameter* is off by one;
  it is asserted at E=2 (also the API default), with an E=1 test pinning the
  other side of the boundary.
- Latency figures are measurement goals to report against, not pass/fail gates.

**Measured on a 100k random-word dictionary** (`trieste_bench`, MinGW g++ 15.2,
Release). Random strings are a pathological corpus — no real prefix structure —
so treat these as an upper bound rather than a forecast:

| Path | Time |
|---|---|
| Exact prefix Top-K (E=0), k=1..20 | 1.5–2.9 µs |
| Fuzzy fallback, E=1, 6-char query | ~101 µs |
| Fuzzy fallback, E=2, 6-char query | ~2.6 ms |

**Open question for M5/M6:** E=2 on a short query is the expensive case, and
`|query| > E` is a weak guard — at `|query| = 3, E = 2` the walk still goes very
wide. Graduated fuzziness (E=0 for ≤2 chars, E=1 for 3–4, E=2 for ≥5, which is
what Elasticsearch's `AUTO` does) would bound it properly. Deferred rather than
guessed: it changes result semantics, so it should be driven by M5 numbers.

## M3 — N-gram context  (done)

- Bigram/trigram Markov model (`NgramModel`) over the corpus for next-token
  prediction.
- Multi-word input: `"san "` → `"francisco"`, ranked by transition probability.
- Blended into `getSuggestions` when the input ends on a word boundary (trailing
  space).

**Decisions taken**

- *Markov order: bigram + trigram.* Unigrams give no context. Bigrams are the
  minimum useful unit; trigrams improve precision for short repeated phrases
  ("new york city"). Higher orders would need much larger corpora to have
  meaningful counts and are deferred to M6 if benchmarks show they help.
- *Training is transitive through `insertQuery`.* `loadCorpus` calls
  `insertQuery`, and `insertQuery` now trains the n-gram model with the same
  normalised token stream it feeds to the trie. No separate training pass and no
  API surface change.
- *Trigram context takes priority.* Trigram candidates fill first; bigram
  candidates fill only the remaining slots (de-duplicated). This matches
  standard back-off intuition: more context → more specific → higher ranked.
- *Word-boundary trigger: trailing space only.* A bare trailing space is
  unambiguous — the user has completed a word and expects what comes next.
  Mid-word input continues to get trie completion (more useful per keystroke).
  More sophisticated triggers (e.g. detecting two-word inputs without the
  trailing space) are deferred to M6.
- *Blending order: exact trie hits, then fuzzy corrections, then n-gram
  predictions.* An n-gram prediction never displaces a term the trie already
  found; de-duplication removes repeats before any candidate enters the result
  list.
- *No probability smoothing.* Raw transition counts are enough for the small
  corpora trieste targets. Laplace or Kneser-Ney smoothing would obscure the
  frequency signal rather than improve it at this scale; deferred if benchmarks
  flag it.

## M4 — Concurrency  (done)

One `std::shared_mutex` guards the trie (and, when added, the n-gram model).
Shared lock on every read path; exclusive lock on every write path.  The
engine was already non-movable since M1 precisely to anticipate this.

**Decisions taken**

- *Single coarse mutex.* Per-node locking or lock-free structures are deferred
  until M5 measures actual contention.  A shared_mutex costs nothing under
  read-dominated load (many readers hold it simultaneously) and is much
  simpler to audit for correctness.
- *I/O outside the lock in `loadCorpus`.* The corpus file is parsed into a
  local `vector<pair<string,int>>` with no lock held; the lock is acquired
  once for the batch insert.  File I/O can be slow (disk, network drive) and
  blocking readers for its duration would be a needless pessimisation.
  The original implementation called `insertQuery` per line, which would
  have acquired and released the lock once per entry — that is replaced by a
  single batch critical section.
- *`mutable` mutex.* Declared `mutable` so that const read-functions
  (`getScoredSuggestions`, `termCount`, `nodeCount`) can acquire the shared
  lock without a `const_cast`.  This is the standard C++ idiom for
  internal synchronisation primitives on logically-const objects.
- *`termCount` / `nodeCount` added to the lock scope.* Although `std::size_t`
  loads are atomic on all supported architectures in practice, the C++ abstract
  machine requires a happens-before edge; the shared lock provides it without
  cost under contention-free conditions.

**Test suite added** — `tests/concurrency_test.cpp`:

| Test | What it checks |
|---|---|
| `ConcurrentReads` | 16 threads × 200 reads; top result always correct |
| `ConcurrentWrites` | 8 threads × 100 inserts; frequency sum preserved exactly |
| `ConcurrentReadWriteMix` | 6 readers + 4 writers simultaneously; no crash, correct final count |
| `LoadCorpusConcurrent` | two `loadCorpus` calls on separate threads; no deadlock, all terms present |
| `MetricsUnderConcurrentWrites` | `termCount`/`nodeCount` callable from any thread |

Run under **ThreadSanitizer** (TSAN) for definitive data-race detection:

```sh
# Only on Linux/macOS with GCC or Clang — TSAN is not available on MSVC/MinGW
cmake --preset unix -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build --preset unix
ctest --preset unix
```

## M5 — Benchmark suite  (done)

Full write-up and charts in [RESULTS.md](RESULTS.md).

**Decisions taken**

- *Realistic corpus, not random strings.* 108,008 terms: 100,000 real English
  words with the frequency head taken from real observed data, Zipfian weights,
  plus 8,000 phrases for the n-gram path. Random strings share no prefixes, so
  the trie degenerates and subtree pruning — the basis of the sub-O(N) claim —
  can never fire. Benchmarking on them measures a pathological case. The corpus
  is committed for reproducibility and regenerable via `tools/corpus_gen.cpp`.
- *Percentiles measured per query, not per repetition.* Google Benchmark's
  built-in repetition statistics give percentiles of per-repetition means, which
  averages away exactly the slow calls a p99 exists to expose.
- *Fair naive baseline.* The naive side computes the same prefix-relaxed
  predicate (min of the final DP row), not whole-word distance, and an
  equivalence benchmark asserts both implementations return identical hit counts.
  A speedup between two different questions would be meaningless.
- *Benchmark families run in isolation.* Running the whole suite back-to-back
  throttled the laptop and inflated later results ~2.3×.

**Results**

| Path | p50 | p99 |
|---|---|---|
| Exact prefix, 4-char, k=5 | 12.6 µs | 323 µs |
| Exact prefix, 2-char, k=5 | 489.6 µs | 3.90 ms |
| Fuzzy E=1 | 40.3 µs | 89.8 µs |
| Fuzzy E=2 | 477.2 µs | 1.32 ms |
| N-gram prediction | 15.9 µs | 237.5 µs |

- **Naive vs pruned: 245× at E=1, 24× at E=2**, 0 mismatches.
- **199,000 QPS at 16 reader threads**; per-thread latency flat from 2→16
  threads, so the shared_mutex read path parallelises essentially perfectly.
- A single continuous writer costs ~4× read throughput.
- Spec targets met everywhere except 2-character prefixes (p95 2.22 ms vs 2 ms).

## M6 — Memory and latency optimisation  (done)

Driven by M5's numbers. Full before/after in [RESULTS.md](RESULTS.md#8-m6--subtree-max-best-first-descent).

**Decisions taken**

- *Subtree-max + best-first descent, over cached per-node Top-K.* Every node
  caches `subtreeMax`, the highest frequency beneath it; the query becomes a
  best-first descent ordered by `(bound desc, path asc)`. Chosen over the
  LeetCode-642 style cached Top-K list, which at 432,062 nodes would have added
  ~41 MB even with 4-byte term IDs — more than doubling a 31.6 MB trie, in a
  milestone whose other goal was to *reduce* memory. The bound costs nothing:
  `Node` was 24 bytes of vector plus a 4-byte int padded to 32, and the second
  int lands in that padding. Measured: 31.6 MB → 31.3 MB.
- *No cache, therefore no invalidation.* The update is one monotonic `max` along
  the insert path. It is exact rather than approximate **only because
  frequencies never decrease** — insert accumulates, non-positive weights are
  rejected, nothing deletes. Adding a delete or weight-decrease API breaks this
  and would require recomputing the affected path bottom-up. Commented at the
  call site.
- *Memory reported, not optimised.* 75.9 bytes/node against a 32-byte struct, so
  ~44 bytes is children-vector allocation plus malloc header — a real 2.4×
  overhead an arena would reclaim. But 31.3 MB for 108k terms hurts nothing, and
  bundling that rewrite here would have destroyed the before/after attribution.

**Results**

| Prefix length | p95 before | p95 after | speedup |
|---|---|---|---|
| 2 chars | 6276.3 µs | **12.4 µs** | **506×** |
| 3 chars | 1404.8 µs | 10.0 µs | 140× |
| 4 chars | 333.9 µs | 7.4 µs | 45× |
| 8 chars | 6.0 µs | 1.9 µs | 3.2× |

- Every prefix length improved — the fast deep-prefix path did not regress.
- Throughput at 16 threads: 39,795 → **992,462 QPS**.
- `insertQuery` p50 0.80 → 0.90 µs (+12.5%), the one real trade: a second O(L)
  descent to maintain the bound.
- Spec targets now met everywhere, with ~160× headroom on the former worst case.
- Ranking proven unchanged: 1,180 differential comparisons against a brute-force
  reference on the full corpus, 0 mismatches, plus 5 new unit tests.

**Still open**

- Arena / compressed nodes, if memory ever becomes a real constraint.
- Graduated fuzziness (open since M2) — for behaviour, not speed.
- A true Levenshtein automaton remains unjustified: the pruned DP walk is
  245×/24× ahead of naive and well inside budget.
