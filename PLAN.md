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

## M1 — Scaffold, repo setup, Trie, exact prefix, Top-K  ✅ done

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

## M2 — Fuzzy search, bounded edit distance (E ≤ 2)  ✅ done

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

## M3 — N-gram context

- Bigram/trigram Markov model over the corpus for next-token prediction.
- Multi-word input: `"san "` → `"francisco"`, ranked by transition probability.
- Blended into `getSuggestions` when the input ends on a word boundary.

## M4 — Concurrency

- Start with **one `std::shared_mutex`**: shared lock on read, exclusive on
  write, guarding the trie and the n-gram model together.
- Only go fine-grained (per-node locking, sharding, or a lock-free read path)
  **if the M5 benchmarks actually show contention.** Note that
  `AutocompleteEngine` is already non-movable for exactly this reason.

## M5 — Benchmark suite

- Latency profile: p50 / p95 / p99, against the spec's budget of p95 < 2 ms and
  p99 < 5 ms on a 100k+ word dictionary.
- Multi-threaded QPS under concurrent read load (and read/write mix).
- Naive full-dictionary O(M×N) Levenshtein DP vs. the trie-pruned walk from M2,
  head-to-head on the same corpus.

## M6 — Memory and latency optimisation

Driven by M5's numbers, not by guesswork.

- Compressed / radix nodes to collapse single-child chains.
- Cached Top-K (or a subtree max-frequency) per node so prefix search can prune
  branches that cannot beat the current heap minimum, instead of walking the
  whole subtree.
- Arena or vector-backed node storage to cut pointer-chasing.
