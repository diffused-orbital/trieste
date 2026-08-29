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

## M2 — Fuzzy search, bounded edit distance (E ≤ 2)

Triggered only when exact prefix matches < K.

- Start with the **DP-row-over-Trie** approach: carry one Levenshtein DP row
  down each trie edge, prune a subtree as soon as `min(row) > maxEditDistance`.
  Correct, easy to reason about, and already asymptotically far better than
  scanning the dictionary.
- Keep a true **Levenshtein Automaton** as a later optimisation, benchmarked
  head-to-head against the DP-row version rather than assumed faster.
- Merge corrections *below* exact matches in the ranking.
- Turns on the disabled `SpecExample.TypoCorrectionArrivesInM2` test:
  `getSuggestions("heloo", k=2, maxEditDistance=1)` → `["hello", "help"]`.

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
