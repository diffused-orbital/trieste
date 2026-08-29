#pragma once

#include <cstddef>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "trieste/trie.hpp"

namespace trieste {

/// The engine's public facade.
///
/// This class's signature is FROZEN as of M1. Later milestones fill in
/// behaviour behind these methods; they do not change the shape of the API.
/// Methods that are not fully implemented yet say so, and are marked TODO(Mx).
///
/// Not copyable and deliberately not movable: M4 adds a std::shared_mutex
/// member, and mutexes are neither. Declaring the moves now and deleting them
/// later would be a silent source break for anything built against M1..M3.
class AutocompleteEngine {
public:
    AutocompleteEngine();
    ~AutocompleteEngine();
    AutocompleteEngine(const AutocompleteEngine&) = delete;
    AutocompleteEngine& operator=(const AutocompleteEngine&) = delete;

    // ---- Spec API ---------------------------------------------------------

    /// Load a corpus file and build the frequency-weighted Trie.
    ///
    /// Accepted line formats, one entry per line:
    ///     term
    ///     term <whitespace> weight
    /// A trailing all-digit token is read as the weight; anything else makes
    /// the whole line the term with weight 1. Splitting on the LAST whitespace
    /// keeps multi-word terms ("san francisco 900") working, which M3 needs.
    /// Blank lines and lines starting with '#' are skipped.
    ///
    /// Entries ACCUMULATE onto whatever is already loaded -- calling this twice
    /// merges two corpora rather than replacing the first.
    ///
    /// Throws std::runtime_error if the file cannot be opened.
    void loadCorpus(const std::string& filePath);

    /// Dynamic update endpoint for continuous learning. Adds `weight` to the
    /// term's running frequency, creating it if new.
    void insertQuery(const std::string& query, int weight = 1);

    /// Primary per-keystroke search.
    ///
    /// M1 behaviour: exact prefix match, ranked Top-K. When fewer than `k`
    /// terms carry the prefix it returns just those -- the typo-tolerant
    /// fallback that fills the gap is M2, and `maxEditDistance` is accepted but
    /// ignored until then.
    [[nodiscard]] std::vector<std::string> getSuggestions(const std::string& inputPrefix,
                                                          int k = 5,
                                                          int maxEditDistance = 2) const;

    // ---- Extras (not in the spec contract; used by tests, CLI, benchmarks) --

    /// The largest edit budget the engine honours, per the spec's E <= 2.
    /// Larger values passed to getSuggestions are clamped to this.
    static constexpr int kMaxEditDistance = 2;

    /// Diagnostics for one query. Filled in only when the caller supplies a
    /// destination -- nothing is cached on the engine, so queries stay const and
    /// remain safe to run concurrently once M4 adds the shared lock.
    struct QueryStats {
        std::size_t exactMatches = 0;  ///< how many the exact prefix pass produced
        bool fuzzyRan = false;         ///< false when the exact pass already met k
        Trie::FuzzyStats fuzzy;        ///< zeroed unless fuzzyRan
    };

    /// Same as getSuggestions but keeps the frequency that earned each slot, and
    /// optionally reports how the answer was reached.
    [[nodiscard]] std::vector<ScoredTerm> getScoredSuggestions(const std::string& inputPrefix,
                                                               int k = 5,
                                                               int maxEditDistance = 2,
                                                               QueryStats* stats = nullptr) const;

    [[nodiscard]] std::size_t termCount() const noexcept;
    [[nodiscard]] std::size_t nodeCount() const noexcept;

    /// Normalisation applied to every term on the way in and every prefix on
    /// the way out, so lookups are symmetric: trims the ends, lowercases ASCII,
    /// and collapses internal whitespace runs to a single space (which keeps
    /// M3's multi-word keys canonical).
    [[nodiscard]] static std::string normalize(std::string_view text);

private:
    mutable std::shared_mutex mutex_;  // M4: shared on read, exclusive on write
    Trie trie_;

    // TODO(M3): NgramModel ngrams_;  bigram/trigram transition counts.
    //           When added, it must be guarded by the same mutex_ as trie_.
};

}  // namespace trieste
