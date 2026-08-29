#include "trieste/autocomplete_engine.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>

namespace trieste {
namespace {

[[nodiscard]] bool isSpace(unsigned char c) noexcept {
    return std::isspace(c) != 0;
}

[[nodiscard]] char toLowerAscii(unsigned char c) noexcept {
    return static_cast<char>(std::tolower(c));
}

/// A trailing token counts as a weight only if it is all digits and short
/// enough to fit an int without overflowing. Anything else -- "apple pie",
/// "covid-19" -- is left as part of the term, which is why we check rather
/// than blindly parsing.
[[nodiscard]] bool isWeightToken(std::string_view token) noexcept {
    if (token.empty() || token.size() > 9) {
        return false;
    }
    for (const char c : token) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

AutocompleteEngine::AutocompleteEngine() = default;
AutocompleteEngine::~AutocompleteEngine() = default;

std::string AutocompleteEngine::normalize(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool pendingSpace = false;
    for (const unsigned char c : text) {
        if (isSpace(c)) {
            // Defer the separator: emitting it only once a non-space follows
            // trims both ends and collapses internal runs in a single pass.
            pendingSpace = !result.empty();
            continue;
        }
        if (pendingSpace) {
            result.push_back(' ');
            pendingSpace = false;
        }
        result.push_back(toLowerAscii(c));
    }
    return result;
}

void AutocompleteEngine::insertQuery(const std::string& query, int weight) {
    // Exclusive: this mutates BOTH the trie and the n-gram model, which the
    // same mutex guards.
    std::unique_lock lock(mutex_);
    const std::string normalised = normalize(query);
    trie_.insert(normalised, weight);
    ngrams_.train(normalised, weight);  // M3: the same normalised token stream
}

void AutocompleteEngine::loadCorpus(const std::string& filePath) {
    // Parse the file without holding the lock so we don't block readers while
    // doing potentially slow I/O.  We accumulate (term, weight) pairs and then
    // acquire the write lock once to insert them all.
    std::ifstream input(filePath);
    if (!input) {
        throw std::runtime_error("trieste: cannot open corpus file: " + filePath);
    }

    std::vector<std::pair<std::string, int>> entries;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();  // tolerate CRLF corpora on POSIX and vice versa
        }

        std::string_view view(line);
        while (!view.empty() && isSpace(static_cast<unsigned char>(view.front()))) {
            view.remove_prefix(1);
        }
        while (!view.empty() && isSpace(static_cast<unsigned char>(view.back()))) {
            view.remove_suffix(1);
        }
        if (view.empty() || view.front() == '#') {
            continue;
        }

        // Split on the LAST whitespace, not the first, so multi-word terms
        // survive intact -- "san francisco 900" is ("san francisco", 900).
        int weight = 1;
        std::string_view term = view;
        if (const auto split = view.find_last_of(" \t"); split != std::string_view::npos) {
            const std::string_view tail = view.substr(split + 1);
            if (isWeightToken(tail)) {
                weight = std::stoi(std::string(tail));
                term = view.substr(0, split);
            }
        }
        entries.emplace_back(normalize(std::string(term)), weight);
    }

    // Single write-lock acquisition for the whole batch.
    //
    // This deliberately calls trie_.insert() and ngrams_.train() directly
    // instead of insertQuery(). insertQuery() takes a unique_lock on the very
    // mutex already held here, and std::shared_mutex is NOT recursive, so
    // routing through it would self-deadlock. The two-line duplication is
    // load-bearing, not an oversight.
    //
    // Both milestones must be fed here: M4 rewrote this loop to bypass
    // insertQuery, which is where M3 hangs its training. Dropping the train()
    // call silently leaves loadCorpus populating the trie but never the n-gram
    // model -- a merge hazard git reports no conflict for.
    std::unique_lock lock(mutex_);  // exclusive — mutates the trie and the n-gram model
    for (const auto& [term, weight] : entries) {
        trie_.insert(term, weight);
        ngrams_.train(term, weight);  // `entries` already holds normalised terms
    }
}

std::vector<ScoredTerm> AutocompleteEngine::getScoredSuggestions(const std::string& inputPrefix,
                                                                 int k,
                                                                 int maxEditDistance,
                                                                 QueryStats* stats) const {
    std::shared_lock lock(mutex_);  // shared — multiple readers may run concurrently
    QueryStats local;
    const auto publish = [&local, stats] {
        if (stats != nullptr) {
            *stats = local;
        }
    };

    if (k <= 0) {
        publish();
        return {};
    }
    const auto limit = static_cast<std::size_t>(k);
    const std::string prefix = normalize(inputPrefix);

    // Pipeline step 1: exact prefix match. This is the fast default and is all
    // the overwhelming majority of keystrokes ever need.
    std::vector<ScoredTerm> results = trie_.topKWithPrefix(prefix, limit);
    local.exactMatches = results.size();

    if (results.size() >= limit) {
        publish();
        return results;  // k satisfied exactly -- the fuzzy walk never runs
    }

    // De-duplication is complete rather than partial here: because the exact
    // pass returned FEWER than k, it necessarily returned every term carrying
    // the prefix, so `results` is the whole exact set and nothing can slip past.
    const auto alreadyPresent = [&results](const std::string& term) {
        return std::any_of(results.begin(), results.end(),
                           [&term](const ScoredTerm& entry) { return entry.term == term; });
    };

    // Pipeline step 2: n-gram context, but only on a word boundary -- trailing
    // whitespace on the RAW input, checked before normalize() trims it away.
    //
    // A trailing space means the user finished a word and wants to know what
    // comes NEXT, so a context prediction answers the actual question better
    // than a typo correction of the word they already typed correctly.
    //
    // ORDERING MATTERS, and this is why predictions are gathered here rather
    // than after the fuzzy pass: on any rich corpus fuzzy will cheerfully fill
    // every free slot with near-miss corrections -- "san " has four exact hits,
    // and fuzzy offers "bank" one edit from "san" -- leaving nothing for the
    // predictions and starving the feature entirely. Predictions take the slots
    // immediately below the exact hits; corrections get whatever is left.
    const bool wordBoundary =
        !inputPrefix.empty() && isSpace(static_cast<unsigned char>(inputPrefix.back()));

    if (wordBoundary) {
        const auto predictions = ngrams_.predict(prefix, limit - results.size());
        for (const ScoredTerm& prediction : predictions) {
            if (results.size() >= limit) {
                break;
            }
            if (!alreadyPresent(prediction.term)) {
                results.push_back(prediction);
            }
        }
        if (results.size() >= limit) {
            publish();
            return results;
        }
    }

    // Pipeline step 3: typo-tolerant fallback, filling whatever slots the exact
    // and prediction passes left empty.
    const int budget = std::min(maxEditDistance, kMaxEditDistance);
    if (budget <= 0) {
        publish();
        return results;
    }

    local.fuzzyRan = true;
    std::vector<FuzzyMatch> corrections = trie_.fuzzySearch(prefix, budget, &local.fuzzy);

    // Closest corrections first, popularity only as the tie-break. Pruning keeps
    // this candidate set small.
    // TODO(M6): a bounded heap would beat a full sort when k is much smaller
    // than the candidate count -- measure in M5 before bothering.
    std::sort(corrections.begin(), corrections.end(),
              [](const FuzzyMatch& lhs, const FuzzyMatch& rhs) { return ranksBefore(lhs, rhs); });

    // Corrections never displace an exact hit or a context prediction, however
    // popular the typo-match happens to be.
    for (const FuzzyMatch& match : corrections) {
        if (results.size() >= limit) {
            break;
        }
        if (alreadyPresent(match.term)) {
            continue;
        }
        results.push_back(ScoredTerm{match.term, match.frequency});
    }

    publish();
    return results;
}

std::vector<std::string> AutocompleteEngine::getSuggestions(const std::string& inputPrefix,
                                                            int k,
                                                            int maxEditDistance) const {
    const auto scored = getScoredSuggestions(inputPrefix, k, maxEditDistance);
    std::vector<std::string> terms;
    terms.reserve(scored.size());
    for (const auto& entry : scored) {
        terms.push_back(entry.term);
    }
    return terms;
}

std::size_t AutocompleteEngine::termCount() const {
    std::shared_lock lock(mutex_);
    return trie_.termCount();
}

std::size_t AutocompleteEngine::nodeCount() const {
    std::shared_lock lock(mutex_);
    return trie_.nodeCount();
}

}  // namespace trieste
