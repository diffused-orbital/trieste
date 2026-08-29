#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "trieste/trie.hpp"  // for ScoredTerm

namespace trieste {

/// Bigram/trigram Markov transition model for next-token prediction.
///
/// Trained on the same normalised token stream that the Trie consumes. Given
/// the last 1–2 tokens of the user's input, it predicts the most likely next
/// token, ranked by transition count.
///
/// Blending strategy (used by AutocompleteEngine::getScoredSuggestions):
///   1. Trigram context  (w[-2], w[-1]) → candidates, if ≥ 2 context tokens.
///   2. Bigram  context  (w[-1])        → candidates, fills remaining slots.
/// Trigram candidates are preferred because they are more specific. Both sets
/// are de-duplicated before being returned.
///
/// The model is deliberately kept simple: raw counts, no smoothing, no
/// backoff probability — the existing frequency-ranked trie handles the
/// long-tail single-token completions and this model only needs to be
/// good enough to improve multi-word suggestions in the common case.
class NgramModel {
public:
    NgramModel()  = default;
    ~NgramModel() = default;

    // Not copyable (same reasoning as AutocompleteEngine: M4 will add a mutex).
    NgramModel(const NgramModel&) = delete;
    NgramModel& operator=(const NgramModel&) = delete;
    NgramModel(NgramModel&&) noexcept = default;
    NgramModel& operator=(NgramModel&&) noexcept = default;

    /// Feed a normalised, whitespace-separated token stream into the model.
    /// `weight` is added to every transition count so that high-frequency
    /// corpus entries contribute proportionally (mirroring Trie::insert).
    /// Empty text and non-positive weights are ignored.
    void train(std::string_view text, int weight = 1);

    /// Predict the top-`k` most likely next tokens given the trailing context
    /// in `context`.
    ///
    /// `context` is the full, already-normalised query string. The model
    /// tokenises it, extracts the last 1–2 tokens, and looks them up in the
    /// trigram table first, falling back to the bigram table. Results are
    /// ranked by transition count (descending), with a lexicographic tie-break.
    ///
    /// Returns {} when k == 0, the model is empty, or no matching context is
    /// found.
    [[nodiscard]] std::vector<ScoredTerm> predict(std::string_view context,
                                                  std::size_t k) const;

    [[nodiscard]] bool empty() const noexcept { return unigram_.empty(); }

    void clear();

private:
    // unigram_[w]       = total frequency of token w across all training text.
    std::unordered_map<std::string, int> unigram_;

    // bigram_[w1][w2]   = count of w2 immediately following w1.
    std::unordered_map<std::string,
        std::unordered_map<std::string, int>> bigram_;

    // trigram_[w1][w2][w3] = count of w3 immediately following (w1, w2).
    std::unordered_map<std::string,
        std::unordered_map<std::string,
            std::unordered_map<std::string, int>>> trigram_;

    /// Split `text` on ASCII spaces (single spaces only — the text is already
    /// normalised by the engine before being fed here).
    [[nodiscard]] static std::vector<std::string> tokenise(std::string_view text);
};

}  // namespace trieste
