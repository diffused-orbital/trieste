#include "trieste/trie.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <utility>

namespace trieste {

bool ranksBefore(const ScoredTerm& lhs, const ScoredTerm& rhs) noexcept {
    if (lhs.frequency != rhs.frequency) {
        return lhs.frequency > rhs.frequency;  // higher weight ranks first
    }
    return lhs.term < rhs.term;  // deterministic tie-break
}

Trie::Trie() : root_(std::make_unique<Node>()) {}
Trie::~Trie() = default;
Trie::Trie(Trie&&) noexcept = default;
Trie& Trie::operator=(Trie&&) noexcept = default;

// Children are sorted by character, so std::lower_bound gives us the insertion
// point and the lookup answer in one O(log c) probe.
const Trie::Node* Trie::findChild(const Node& node, char c) noexcept {
    const auto it = std::lower_bound(
        node.children.begin(), node.children.end(), c,
        [](const auto& entry, char probe) { return entry.first < probe; });
    if (it == node.children.end() || it->first != c) {
        return nullptr;
    }
    return it->second.get();
}

Trie::Node& Trie::findOrCreateChild(Node& node, char c, std::size_t& nodeCount) {
    const auto it = std::lower_bound(
        node.children.begin(), node.children.end(), c,
        [](const auto& entry, char probe) { return entry.first < probe; });
    if (it != node.children.end() && it->first == c) {
        return *it->second;
    }
    // Insert at the lower_bound position to keep the vector sorted. This shifts
    // the tail, but c is small enough that the memmove beats a hashtable probe
    // in practice -- and inserts are far rarer than the per-keystroke reads.
    const auto inserted = node.children.emplace(it, c, std::make_unique<Node>());
    ++nodeCount;
    return *inserted->second;
}

void Trie::insert(std::string_view word, int weight) {
    if (word.empty() || weight <= 0) {
        return;
    }
    Node* current = root_.get();
    for (const char c : word) {
        current = &findOrCreateChild(*current, c, nodeCount_);
    }
    if (current->frequency == 0) {
        ++termCount_;  // first time we have seen this exact term
    }
    current->frequency += weight;  // accumulate, don't overwrite
}

const Trie::Node* Trie::descend(std::string_view prefix) const noexcept {
    const Node* current = root_.get();
    for (const char c : prefix) {
        current = findChild(*current, c);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

int Trie::frequencyOf(std::string_view word) const {
    const Node* node = descend(word);
    return node != nullptr ? node->frequency : 0;
}

void Trie::collectInto(const Node& node, std::string& path, std::size_t k, TopKHeap& heap) {
    if (node.frequency > 0) {
        if (heap.size() < k) {
            heap.push(ScoredTerm{path, node.frequency});
        } else if (ranksBefore(ScoredTerm{path, node.frequency}, heap.top())) {
            // The heap is full and this candidate beats the weakest survivor:
            // evict, then admit. Candidates that lose here cost one comparison
            // and no allocation.
            heap.pop();
            heap.push(ScoredTerm{path, node.frequency});
        }
    }
    // Alphabetical order, because `children` is sorted -- the walk is stable.
    for (const auto& [character, child] : node.children) {
        path.push_back(character);
        collectInto(*child, path, k, heap);
        path.pop_back();  // reuse one buffer for the whole traversal
    }
}

std::vector<ScoredTerm> Trie::topKWithPrefix(std::string_view prefix, std::size_t k) const {
    if (k == 0) {
        return {};
    }
    const Node* start = descend(prefix);  // O(L) in the prefix length
    if (start == nullptr) {
        return {};
    }

    // M1 scans the whole subtree under the prefix. That is the "correct and
    // simple" baseline; TODO(M6) caches a per-node Top-K (or a subtree max
    // frequency) so this walk can prune branches that cannot beat heap.top().
    TopKHeap heap;
    std::string path(prefix);
    collectInto(*start, path, k, heap);

    std::vector<ScoredTerm> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        results.push_back(heap.top());  // drains weakest-first
        heap.pop();
    }
    std::reverse(results.begin(), results.end());  // ...so reverse to best-first
    return results;
}

void Trie::clear() {
    root_ = std::make_unique<Node>();
    termCount_ = 0;
    nodeCount_ = 1;
}

// ---------------------------------------------------------------------------
// Fuzzy search: one Levenshtein DP row carried down each trie edge.
// ---------------------------------------------------------------------------

bool ranksBefore(const FuzzyMatch& lhs, const FuzzyMatch& rhs) noexcept {
    if (lhs.distance != rhs.distance) {
        return lhs.distance < rhs.distance;  // fewer edits always wins
    }
    if (lhs.frequency != rhs.frequency) {
        return lhs.frequency > rhs.frequency;
    }
    return lhs.term < rhs.term;
}

namespace {

/// "No prefix on this path has come within budget yet." Half of INT_MAX so that
/// std::min against a real distance can never overflow on the way back.
constexpr int kNoMatch = std::numeric_limits<int>::max() / 2;

/// Carry one Levenshtein DP row down a single trie edge.
///
/// Think of the classic |query| x |term| DP grid. A trie node stands for some
/// string S, and `previous` is that grid's column for S: previous[i] is
/// ED(query[0..i), S). Walking the edge labelled `edge` moves to S + edge, so
/// only ONE new column has to be filled in, `out`.
///
/// That is the whole reason the trie beats a dictionary scan. Every node in a
/// subtree reuses the columns already computed for its ancestors, so a term of
/// length n costs O(|query|) here instead of O(|query| * n) from scratch, and
/// shared prefixes pay that cost once on behalf of all their completions.
void advanceRow(const std::vector<int>& previous, char edge, std::string_view query,
                std::vector<int>& out) {
    const std::size_t m = query.size();

    // Row 0 is the empty query against S + edge: one more deletion than before.
    out[0] = previous[0] + 1;

    for (std::size_t i = 1; i <= m; ++i) {
        const int substitute = previous[i - 1] + (query[i - 1] == edge ? 0 : 1);
        const int insert = previous[i] + 1;   // consume the edge character
        const int remove = out[i - 1] + 1;    // consume a query character
        out[i] = std::min({substitute, insert, remove});
    }
}

}  // namespace

struct Trie::FuzzyWalk {
    std::string_view query;
    int maxDistance = 0;

    /// One DP row per depth, reused for the whole traversal rather than
    /// allocated per node. A deque, not a vector: growing it must not
    /// invalidate the row references held by frames further up the recursion,
    /// and vector reallocation would dangle every one of them.
    std::deque<std::vector<int>> rows;

    std::vector<FuzzyMatch> matches;
    FuzzyStats stats;
};

void Trie::collectSubtree(const Node& node, std::string& path, int distance, FuzzyWalk& walk) {
    ++walk.stats.nodesVisited;
    if (node.frequency > 0) {
        walk.matches.push_back(FuzzyMatch{path, node.frequency, distance});
    }
    for (const auto& [character, child] : node.children) {
        path.push_back(character);
        collectSubtree(*child, path, distance, walk);
        path.pop_back();
    }
}

void Trie::fuzzyDescend(const Node& node, std::string& path, std::size_t depth,
                        int inheritedBest, FuzzyWalk& walk) {
    ++walk.stats.nodesVisited;

    const std::size_t m = walk.query.size();
    const std::vector<int>& row = walk.rows[depth];  // deque: stable across the recursion

    // row[m] is ED(query, this node's string) -- what it costs to correct the
    // query into exactly this prefix.
    const int distanceHere = row[m] <= walk.maxDistance ? row[m] : kNoMatch;

    // Best qualifying prefix anywhere on the root -> here path. Once some
    // ancestor has qualified, this node is a completion of that correction and
    // inherits its distance even if its own row has drifted out of budget.
    const int best = std::min(inheritedBest, distanceHere);

    if (best <= walk.maxDistance && node.frequency > 0) {
        walk.matches.push_back(FuzzyMatch{path, node.frequency, best});
    }

    // THE PRUNE, and the reason this is sub-O(N).
    //
    // Every cell of a child's row is >= the minimum cell of this row: each of
    // the three DP transitions either copies a neighbouring cell or adds one to
    // it, so values can never fall below the current floor as we descend. Once
    // that floor exceeds the budget, NO descendant can bring row[m] back inside
    // it, so the entire subtree is unreachable and we abandon it unexamined.
    const int rowFloor = *std::min_element(row.begin(), row.end());

    if (rowFloor > walk.maxDistance) {
        if (best <= walk.maxDistance) {
            // A prefix already qualified, so everything below is a legitimate
            // completion of that correction. No descendant can beat `best`
            // (nothing down there can qualify on its own), so stop computing
            // rows and sweep the subtree flat instead.
            for (const auto& [character, child] : node.children) {
                path.push_back(character);
                collectSubtree(*child, path, best, walk);
                path.pop_back();
            }
        } else {
            ++walk.stats.subtreesPruned;
        }
        return;
    }

    if (walk.rows.size() <= depth + 1) {
        walk.rows.emplace_back(m + 1);
    }
    for (const auto& [character, child] : node.children) {
        advanceRow(walk.rows[depth], character, walk.query, walk.rows[depth + 1]);
        path.push_back(character);
        fuzzyDescend(*child, path, depth + 1, best, walk);
        path.pop_back();
    }
}

std::vector<FuzzyMatch> Trie::fuzzySearch(std::string_view query, int maxDistance,
                                          FuzzyStats* stats) const {
    FuzzyWalk walk;
    walk.query = query;
    walk.maxDistance = maxDistance;

    const std::size_t m = query.size();

    // Guard rails, explained on the declaration. The third is the important
    // one: if the budget covers the whole query then the empty prefix matches,
    // and every term in the dictionary would qualify.
    if (m == 0 || maxDistance <= 0 || m <= static_cast<std::size_t>(maxDistance)) {
        if (stats != nullptr) {
            *stats = walk.stats;
        }
        return {};
    }

    // The root stands for the empty string, so ED(query[0..i), "") == i.
    walk.rows.emplace_back(m + 1);
    for (std::size_t i = 0; i <= m; ++i) {
        walk.rows[0][i] = static_cast<int>(i);
    }

    std::string path;
    fuzzyDescend(*root_, path, 0, kNoMatch, walk);

    if (stats != nullptr) {
        *stats = walk.stats;
    }
    return std::move(walk.matches);
}

}  // namespace trieste
