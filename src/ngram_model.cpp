#include "trieste/ngram_model.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace trieste {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<std::string> NgramModel::tokenise(std::string_view text) {
    std::vector<std::string> tokens;
    std::string_view::size_type start = 0;
    while (start < text.size()) {
        const auto end = text.find(' ', start);
        if (end == std::string_view::npos) {
            tokens.emplace_back(text.substr(start));
            break;
        }
        if (end > start) {
            tokens.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return tokens;
}

// ---------------------------------------------------------------------------
// Training
// ---------------------------------------------------------------------------

void NgramModel::train(std::string_view text, int weight) {
    if (text.empty() || weight <= 0) {
        return;
    }

    const auto tokens = tokenise(text);
    if (tokens.empty()) {
        return;
    }

    // Unigrams
    for (const auto& t : tokens) {
        unigram_[t] += weight;
    }

    // Bigrams
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        bigram_[tokens[i - 1]][tokens[i]] += weight;
    }

    // Trigrams
    for (std::size_t i = 2; i < tokens.size(); ++i) {
        trigram_[tokens[i - 2]][tokens[i - 1]][tokens[i]] += weight;
    }
}

// ---------------------------------------------------------------------------
// Prediction
// ---------------------------------------------------------------------------

void NgramModel::clear() {
    unigram_.clear();
    bigram_.clear();
    trigram_.clear();
}

std::vector<ScoredTerm> NgramModel::predict(std::string_view context,
                                            std::size_t k) const {
    if (k == 0 || empty()) {
        return {};
    }

    // The context arriving here is the full normalised query. Strip the
    // trailing space (word-boundary sentinel) and tokenise what remains.
    std::string_view trimmed = context;
    while (!trimmed.empty() && trimmed.back() == ' ') {
        trimmed.remove_suffix(1);
    }

    const auto tokens = tokenise(trimmed);
    if (tokens.empty()) {
        return {};
    }

    std::vector<ScoredTerm> results;
    results.reserve(k);

    // Collect candidates from a next-token map into a local vector, sort by
    // count descending (lexicographic tie-break), then drain into `results`
    // skipping terms already present.
    const auto drain = [&](const std::unordered_map<std::string, int>& nextMap) {
        if (nextMap.empty()) {
            return;
        }
        // Snapshot and sort the candidates.
        std::vector<ScoredTerm> candidates;
        candidates.reserve(nextMap.size());
        for (const auto& [word, count] : nextMap) {
            candidates.push_back(ScoredTerm{word, count});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const ScoredTerm& a, const ScoredTerm& b) {
                      return ranksBefore(a, b);
                  });

        const auto alreadyPresent = [&](const std::string& term) {
            for (const auto& r : results) {
                if (r.term == term) return true;
            }
            return false;
        };

        for (auto& c : candidates) {
            if (results.size() >= k) break;
            if (!alreadyPresent(c.term)) {
                results.push_back(std::move(c));
            }
        }
    };

    // 1. Trigram lookup — most specific, try first.
    if (tokens.size() >= 2) {
        const auto& w1 = tokens[tokens.size() - 2];
        const auto& w2 = tokens[tokens.size() - 1];
        const auto t1 = trigram_.find(w1);
        if (t1 != trigram_.end()) {
            const auto t2 = t1->second.find(w2);
            if (t2 != t1->second.end()) {
                drain(t2->second);
            }
        }
    }

    // 2. Bigram lookup — falls back to or supplements trigram results.
    if (results.size() < k) {
        const auto& w = tokens.back();
        const auto it = bigram_.find(w);
        if (it != bigram_.end()) {
            drain(it->second);
        }
    }

    return results;
}

}  // namespace trieste
