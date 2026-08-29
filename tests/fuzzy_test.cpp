#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "trieste/autocomplete_engine.hpp"
#include "trieste/trie.hpp"

namespace {

using trieste::AutocompleteEngine;
using trieste::FuzzyMatch;
using trieste::Trie;

/// Fuzzy hits come back in traversal order; sort them for stable comparison.
std::vector<std::string> sortedTerms(const std::vector<FuzzyMatch>& matches) {
    std::vector<std::string> terms;
    terms.reserve(matches.size());
    for (const auto& match : matches) {
        terms.push_back(match.term);
    }
    std::sort(terms.begin(), terms.end());
    return terms;
}

int distanceOf(const std::vector<FuzzyMatch>& matches, const std::string& term) {
    const auto it = std::find_if(matches.begin(), matches.end(),
                                 [&term](const FuzzyMatch& m) { return m.term == term; });
    return it == matches.end() ? -1 : it->distance;
}

Trie helCorpus() {
    Trie trie;
    trie.insert("hello", 900);
    trie.insert("help", 850);
    trie.insert("held", 330);
    trie.insert("helmet", 120);
    trie.insert("helicopter", 210);
    return trie;
}

// ---- Edit distance boundaries -------------------------------------------

TEST(TrieFuzzy, FindsSingleSubstitution) {
    Trie trie;
    trie.insert("hello", 900);
    const auto matches = trie.fuzzySearch("heloo", 1);
    EXPECT_EQ(sortedTerms(matches), (std::vector<std::string>{"hello"}));
    EXPECT_EQ(distanceOf(matches, "hello"), 1);
}

TEST(TrieFuzzy, BudgetOfOneExcludesADistanceTwoTerm) {
    Trie trie;
    trie.insert("hello", 900);
    trie.insert("help", 850);
    // ED("heloo","help") == 2: one substitution plus one deletion.
    EXPECT_EQ(sortedTerms(trie.fuzzySearch("heloo", 1)), (std::vector<std::string>{"hello"}));
}

TEST(TrieFuzzy, BudgetOfTwoAdmitsIt) {
    Trie trie;
    trie.insert("hello", 900);
    trie.insert("help", 850);
    const auto matches = trie.fuzzySearch("heloo", 2);
    EXPECT_EQ(sortedTerms(matches), (std::vector<std::string>{"hello", "help"}));
    EXPECT_EQ(distanceOf(matches, "hello"), 1);
    EXPECT_EQ(distanceOf(matches, "help"), 2);
}

TEST(TrieFuzzy, HandlesInsertionAndDeletionNotJustSubstitution) {
    Trie trie;
    trie.insert("apple", 500);
    // "aple" is one INSERTION away from "apple"...
    EXPECT_EQ(distanceOf(trie.fuzzySearch("aple", 1), "apple"), 1);
    // ...and "appple" is one DELETION away.
    EXPECT_EQ(distanceOf(trie.fuzzySearch("appple", 1), "apple"), 1);
}

TEST(TrieFuzzy, ReportsTheSmallestQualifyingPrefixDistance) {
    const Trie trie = helCorpus();
    // ED("helo","hel") == 1 qualifies the whole "hel" subtree, and nothing in
    // it can do better than 1, so every hit reports exactly 1.
    const auto matches = trie.fuzzySearch("helo", 1);
    for (const auto& match : matches) {
        EXPECT_EQ(match.distance, 1) << match.term;
    }
}

// ---- Fuzzy-prefix semantics: correct, then complete ----------------------

TEST(TrieFuzzy, CorrectedPrefixDragsItsWholeSubtreeAlong) {
    const Trie trie = helCorpus();
    // "hel" is one edit from "helo", so every completion of "hel" is a valid
    // suggestion -- including "helicopter", which as a whole word is nowhere
    // near "helo". This is exactly what whole-term matching could not do.
    EXPECT_EQ(sortedTerms(trie.fuzzySearch("helo", 1)),
              (std::vector<std::string>{"held", "helicopter", "hello", "helmet", "help"}));
}

TEST(TrieFuzzy, DoesNotReachTermsBeyondTheBudget) {
    Trie trie;
    trie.insert("hello", 900);
    trie.insert("zebra", 100);
    trie.insert("banana", 400);
    EXPECT_EQ(sortedTerms(trie.fuzzySearch("heloo", 2)), (std::vector<std::string>{"hello"}));
}

TEST(TrieFuzzy, WorksOverSpacesAndDigitsNotJustLetters) {
    // The sorted-vector node accepts the full char range, and so must the DP
    // walk -- a fixed a-z alphabet would break every case here.
    Trie trie;
    trie.insert("san francisco", 900);
    trie.insert("covid-19", 500);
    EXPECT_EQ(distanceOf(trie.fuzzySearch("san franzisco", 1), "san francisco"), 1);
    EXPECT_EQ(distanceOf(trie.fuzzySearch("covid-18", 1), "covid-19"), 1);
    // A dropped space is just a deletion.
    EXPECT_EQ(distanceOf(trie.fuzzySearch("sanfrancisco", 1), "san francisco"), 1);
}

// ---- Pruning -------------------------------------------------------------

TEST(TrieFuzzy, PruningAbandonsUnreachableBranches) {
    Trie trie;
    for (const char* word : {"hello", "help", "held", "helmet", "helicopter",
                             "banana", "band", "bandwidth", "bank", "banker",
                             "zebra", "zulu", "zeppelin", "zenith",
                             "cat", "cattle", "catalog", "cauldron"}) {
        trie.insert(word, 100);
    }

    Trie::FuzzyStats stats;
    const auto matches = trie.fuzzySearch("helo", 1, &stats);

    // Only the "hel" family is reachable within one edit.
    EXPECT_EQ(sortedTerms(matches),
              (std::vector<std::string>{"held", "helicopter", "hello", "helmet", "help"}));

    // The b/c/z branches must be abandoned rather than walked: a character or
    // two in, the row floor exceeds the budget and everything below is skipped.
    EXPECT_GT(stats.subtreesPruned, 0u);
    EXPECT_LT(stats.nodesVisited, trie.nodeCount());
}

TEST(TrieFuzzy, PruningKeepsWorkFlatAsUnrelatedTermsAreAdded) {
    // Terms sharing no prefix with the query must not add deep traversal work.
    // That is the entire claim of subtree pruning.
    Trie trie;
    trie.insert("hello", 1);
    trie.insert("help", 1);

    Trie::FuzzyStats before;
    (void)trie.fuzzySearch("heloo", 1, &before);

    for (const char* word : {"zebra", "zulu", "quantum", "quixotic", "xylophone",
                             "yacht", "walrus", "vortex", "umbrella", "trombone"}) {
        trie.insert(word, 1);
    }
    Trie::FuzzyStats after;
    (void)trie.fuzzySearch("heloo", 1, &after);

    // Each unrelated first letter costs one visited node before its subtree is
    // pruned, so the growth lands in the pruned count, not in deep traversal.
    EXPECT_GT(after.subtreesPruned, before.subtreesPruned);
    EXPECT_LT(after.nodesVisited, trie.nodeCount());
}

// ---- Guard rails ---------------------------------------------------------

TEST(TrieFuzzy, ReturnsNothingForADisabledOrEmptyQuery) {
    const Trie trie = helCorpus();
    EXPECT_TRUE(trie.fuzzySearch("hello", 0).empty());   // budget disabled
    EXPECT_TRUE(trie.fuzzySearch("hello", -1).empty());
    EXPECT_TRUE(trie.fuzzySearch("", 2).empty());        // nothing to correct
}

TEST(TrieFuzzy, RefusesQueriesNoLongerThanTheBudget) {
    const Trie trie = helCorpus();
    // With a budget at least the query length, the EMPTY prefix is itself
    // within budget, which would qualify the entire dictionary. The guard
    // declines instead of degenerating into a full scan.
    Trie::FuzzyStats stats;
    EXPECT_TRUE(trie.fuzzySearch("he", 2, &stats).empty());
    EXPECT_EQ(stats.nodesVisited, 0u);
    EXPECT_FALSE(trie.fuzzySearch("hel", 2).empty());  // one character longer: allowed
}

TEST(TrieFuzzy, StatsArgumentIsOptional) {
    const Trie trie = helCorpus();
    EXPECT_FALSE(trie.fuzzySearch("heloo", 2, nullptr).empty());
}

// ---- Engine level: when fuzzy fires, and how it merges -------------------

TEST(EngineFuzzy, StaysOffWhenExactMatchesAlreadySatisfyK) {
    AutocompleteEngine engine;
    engine.insertQuery("apple", 500);
    engine.insertQuery("apply", 300);
    engine.insertQuery("application", 1000);

    AutocompleteEngine::QueryStats stats;
    const auto results = engine.getScoredSuggestions("appl", /*k=*/2, /*E=*/2, &stats);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(stats.exactMatches, 2u);
    EXPECT_FALSE(stats.fuzzyRan);
    EXPECT_EQ(stats.fuzzy.nodesVisited, 0u);  // the walk never started
}

TEST(EngineFuzzy, FiresOnlyWhenExactMatchesFallShortOfK) {
    AutocompleteEngine engine;
    engine.insertQuery("apple", 500);
    engine.insertQuery("apply", 300);

    AutocompleteEngine::QueryStats stats;
    const auto results = engine.getScoredSuggestions("appl", /*k=*/5, /*E=*/2, &stats);

    EXPECT_EQ(stats.exactMatches, 2u);
    EXPECT_TRUE(stats.fuzzyRan);
    EXPECT_GT(stats.fuzzy.nodesVisited, 0u);
    EXPECT_GE(results.size(), 2u);
}

TEST(EngineFuzzy, ExactMatchesAreNeverDisplacedByCorrections) {
    AutocompleteEngine engine;
    engine.insertQuery("apple", 10);       // exact match, almost no weight
    engine.insertQuery("aple", 100000);    // one edit away, enormous weight
    // Outweighing the exact hit ten-thousand-fold still does not promote the
    // correction. Exact-first is what stops typing another character from
    // pushing the term you actually wanted DOWN the list.
    EXPECT_EQ(engine.getSuggestions("appl", /*k=*/5, /*E=*/2),
              (std::vector<std::string>{"apple", "aple"}));
}

TEST(EngineFuzzy, CorrectionsAreOrderedByDistanceThenFrequency) {
    AutocompleteEngine engine;
    engine.insertQuery("hello", 10);    // distance 1 from "heloo", tiny weight
    engine.insertQuery("help", 90000);  // distance 2, huge weight
    // The closer correction ranks first despite being far less popular.
    EXPECT_EQ(engine.getSuggestions("heloo", /*k=*/5, /*E=*/2),
              (std::vector<std::string>{"hello", "help"}));
}

TEST(EngineFuzzy, DoesNotRepeatATermFoundByBothPasses) {
    AutocompleteEngine engine;
    engine.insertQuery("apple", 500);
    engine.insertQuery("apply", 300);
    // "apple" is an exact prefix hit AND within one edit of "appl", so it is a
    // candidate twice over. It must appear exactly once.
    const auto results = engine.getSuggestions("appl", /*k=*/5, /*E=*/2);
    EXPECT_EQ(std::count(results.begin(), results.end(), std::string("apple")), 1);
}

TEST(EngineFuzzy, HonoursKAsAHardCap) {
    AutocompleteEngine engine;
    for (const char* word : {"hello", "help", "held", "helmet", "helicopter"}) {
        engine.insertQuery(word, 100);
    }
    EXPECT_EQ(engine.getSuggestions("helo", /*k=*/2, /*E=*/1).size(), 2u);
}

TEST(EngineFuzzy, BudgetIsClampedToTheSpecMaximum) {
    AutocompleteEngine engine;
    engine.insertQuery("hello", 900);
    // A caller asking for nine edits gets two. Without the clamp a large budget
    // on a short query would drag in the whole dictionary.
    EXPECT_EQ(engine.getSuggestions("hellooo", /*k=*/5, /*E=*/9),
              engine.getSuggestions("hellooo", /*k=*/5, /*E=*/2));
}

TEST(EngineFuzzy, ZeroBudgetDisablesCorrectionEntirely) {
    AutocompleteEngine engine;
    engine.insertQuery("hello", 900);
    AutocompleteEngine::QueryStats stats;
    const auto results = engine.getScoredSuggestions("heloo", /*k=*/5, /*E=*/0, &stats);
    EXPECT_TRUE(results.empty());
    EXPECT_FALSE(stats.fuzzyRan);
}

TEST(EngineFuzzy, CorrectionRespectsNormalisation) {
    AutocompleteEngine engine;
    engine.insertQuery("Hello", 900);
    // Query and corpus are both normalised, so case never costs an edit.
    EXPECT_EQ(engine.getSuggestions("HELOO", /*k=*/1, /*E=*/1),
              (std::vector<std::string>{"hello"}));
}

TEST(EngineFuzzy, CorrectsAgainstTheShippedSampleCorpus) {
    AutocompleteEngine engine;
    engine.loadCorpus(std::string(TRIESTE_DATA_DIR) + "/sample_corpus.txt");
    // "helo" reaches the hel* family; at equal distance frequency decides.
    EXPECT_EQ(engine.getSuggestions("helo", /*k=*/2, /*E=*/1),
              (std::vector<std::string>{"hello", "help"}));
}

}  // namespace
