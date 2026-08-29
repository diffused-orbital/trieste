#pragma once

#include <cstddef>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trieste {

/// A dictionary term paired with the accumulated frequency that ranks it.
/// Queries return these (rather than bare strings) so callers -- tests,
/// benchmarks, the CLI -- can see *why* an entry won its slot.
struct ScoredTerm {
    std::string term;
    int frequency = 0;

    friend bool operator==(const ScoredTerm&, const ScoredTerm&) = default;
};

/// The single ranking order used everywhere in the engine.
///
/// Higher frequency wins. Ties break lexicographically (smaller term first) so
/// that an identical corpus always yields byte-identical output -- without a
/// total order, Top-K results would wobble between runs and neither the tests
/// nor the M5 benchmarks would be reproducible.
[[nodiscard]] bool ranksBefore(const ScoredTerm& lhs, const ScoredTerm& rhs) noexcept;

/// Frequency-weighted prefix tree.
///
/// CHILD REPRESENTATION (design fork, decided in M1): every node stores its
/// children in a std::vector kept sorted by character and searched with binary
/// search. Versus the textbook unordered_map node this gives up O(1) hashing
/// for O(log c) over a tiny c -- real nodes have a handful of children, not 26 --
/// and buys back three things that matter downstream:
///   1. dense, cache-friendly storage (one allocation per node, not a hashtable);
///   2. no per-node hashtable overhead, which is what the M6 memory work targets;
///   3. deterministic alphabetical iteration, which M2's fuzzy DFS and the
///      tie-break rule above both depend on.
class Trie {
public:
    Trie();
    ~Trie();
    Trie(Trie&&) noexcept;
    Trie& operator=(Trie&&) noexcept;
    Trie(const Trie&) = delete;
    Trie& operator=(const Trie&) = delete;

    /// Insert `word`, ADDING `weight` to whatever frequency it already carries.
    /// Re-inserting an existing term is how the engine learns from live traffic.
    /// Empty words and non-positive weights are ignored.
    void insert(std::string_view word, int weight);

    /// Up to `k` terms starting with `prefix`, best-ranked first.
    /// An empty prefix ranks the whole dictionary. Returns {} when k == 0 or no
    /// term carries the prefix.
    [[nodiscard]] std::vector<ScoredTerm> topKWithPrefix(std::string_view prefix, std::size_t k) const;

    /// Accumulated frequency of an exact term; 0 if it was never inserted.
    [[nodiscard]] int frequencyOf(std::string_view word) const;
    [[nodiscard]] bool contains(std::string_view word) const { return frequencyOf(word) > 0; }

    [[nodiscard]] std::size_t termCount() const noexcept { return termCount_; }
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodeCount_; }
    [[nodiscard]] bool empty() const noexcept { return termCount_ == 0; }

    void clear();

private:
    struct Node {
        /// Sorted by `first`. unique_ptr ties a child's lifetime to its parent,
        /// so the whole trie tears down recursively with no manual walk.
        std::vector<std::pair<char, std::unique_ptr<Node>>> children;
        /// > 0 marks the end of a stored term and carries its weight.
        int frequency = 0;
    };

    /// Bounded min-heap for Top-K selection.
    ///
    /// std::priority_queue exposes the GREATEST element under its comparator at
    /// top(). Feeding it `ranksBefore` -- "lhs outranks rhs" -- therefore parks
    /// the WEAKEST surviving candidate at top(), which is exactly the element we
    /// want to evict. That inversion is the whole trick: the heap never grows
    /// past k, so scanning a subtree of n terms costs O(n log k) time and O(k)
    /// space instead of sorting all n.
    struct WeakestOnTop {
        bool operator()(const ScoredTerm& lhs, const ScoredTerm& rhs) const noexcept {
            return ranksBefore(lhs, rhs);
        }
    };
    using TopKHeap = std::priority_queue<ScoredTerm, std::vector<ScoredTerm>, WeakestOnTop>;

    [[nodiscard]] static const Node* findChild(const Node& node, char c) noexcept;
    [[nodiscard]] static Node& findOrCreateChild(Node& node, char c, std::size_t& nodeCount);

    /// Walk `prefix` from the root. Returns nullptr if the path does not exist.
    [[nodiscard]] const Node* descend(std::string_view prefix) const noexcept;

    /// Depth-first walk of a subtree, offering every terminal node to `heap`.
    /// `path` is a reusable buffer carrying the characters spelled so far, so
    /// the walk allocates a string only for candidates that actually enter the
    /// heap rather than for every node visited.
    static void collectInto(const Node& node, std::string& path, std::size_t k, TopKHeap& heap);

    std::unique_ptr<Node> root_;
    std::size_t termCount_ = 0;
    std::size_t nodeCount_ = 1;  // the root itself
};

}  // namespace trieste
