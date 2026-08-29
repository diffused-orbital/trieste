#include "trieste/trie.hpp"

#include <algorithm>
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

}  // namespace trieste
