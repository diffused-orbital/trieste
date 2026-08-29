// Tests for M3: NgramModel and engine-level n-gram blending.
//
// Covers:
//   - NgramModel basic bigram prediction
//   - Trigram overrides / supplements bigram
//   - Engine: getSuggestions with trailing space triggers n-gram predictions
//   - Word boundary heuristic: no blending without trailing space
//   - k cap is respected
//   - De-duplication: n-gram doesn't repeat trie hits
//   - loadCorpus trains the n-gram model automatically
//   - Empty model returns nothing

#include "trieste/autocomplete_engine.hpp"
#include "trieste/ngram_model.hpp"
#include "trieste/trie.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using trieste::AutocompleteEngine;
using trieste::NgramModel;
using trieste::ScoredTerm;

// ---- NgramModel unit tests -------------------------------------------------

TEST(NgramModel, StartsEmpty) {
    const NgramModel model;
    EXPECT_TRUE(model.empty());
    EXPECT_TRUE(model.predict("hello ", 5).empty());
}

TEST(NgramModel, PredictReturnsNothingForZeroK) {
    NgramModel model;
    model.train("san francisco", 900);
    EXPECT_TRUE(model.predict("san ", 0).empty());
}

TEST(NgramModel, BasicBigramPrediction) {
    NgramModel model;
    model.train("san francisco", 900);
    model.train("san diego", 450);

    const auto results = model.predict("san ", 5);
    ASSERT_FALSE(results.empty());

    std::vector<std::string> terms;
    for (const auto& r : results) terms.push_back(r.term);
    EXPECT_EQ(terms, (std::vector<std::string>{"francisco", "diego"}));
}

TEST(NgramModel, BigramRankedByCountDescending) {
    NgramModel model;
    model.train("go home", 100);
    model.train("go left", 500);
    model.train("go right", 300);

    const auto results = model.predict("go ", 3);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].term, "left");   // 500
    EXPECT_EQ(results[1].term, "right");  // 300
    EXPECT_EQ(results[2].term, "home");   // 100
}

TEST(NgramModel, BigramLexicographicTieBreak) {
    NgramModel model;
    // Both follow "go" with equal count.
    model.train("go alpha", 100);
    model.train("go beta", 100);

    const auto results = model.predict("go ", 2);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].term, "alpha");  // lexicographically first
    EXPECT_EQ(results[1].term, "beta");
}

TEST(NgramModel, TrigramTakesPriorityOverBigram) {
    NgramModel model;
    // Trigram: "new" "york" → "city" (high count via trigram)
    model.train("new york city", 1000);
    // Bigram: "york" → "times" (would win if we only used bigram)
    model.train("york times", 9999);

    // With trigram context "new york ", "city" should appear via the trigram
    // before "times" (which would come from the plain bigram "york" → "times").
    const auto results = model.predict("new york ", 5);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].term, "city");
}

TEST(NgramModel, FallsBackToBigramWhenNoTrigramContextExists) {
    NgramModel model;
    model.train("san francisco", 900);  // trains bigram "san"→"francisco"
    // No trigram with "foo san" context.
    const auto results = model.predict("foo san ", 5);
    // Should fall back to bigram "san"→"francisco".
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].term, "francisco");
}

TEST(NgramModel, ReturnsNothingForUnknownContext) {
    NgramModel model;
    model.train("san francisco", 900);
    EXPECT_TRUE(model.predict("xyz ", 5).empty());
}

TEST(NgramModel, TrainAccumulatesWeight) {
    NgramModel model;
    model.train("go left", 100);
    model.train("go left", 200);
    model.train("go right", 250);

    const auto results = model.predict("go ", 2);
    ASSERT_EQ(results.size(), 2u);
    // "left" accumulated 300, beats "right" at 250.
    EXPECT_EQ(results[0].term, "left");
    EXPECT_EQ(results[0].frequency, 300);
    EXPECT_EQ(results[1].term, "right");
}

TEST(NgramModel, ClearResetsModel) {
    NgramModel model;
    model.train("san francisco", 900);
    EXPECT_FALSE(model.empty());
    model.clear();
    EXPECT_TRUE(model.empty());
    EXPECT_TRUE(model.predict("san ", 5).empty());
}

TEST(NgramModel, KCapIsRespected) {
    NgramModel model;
    for (const char* w : {"alpha", "beta", "gamma", "delta", "epsilon"}) {
        model.train(std::string("go ") + w, 100);
    }
    const auto results = model.predict("go ", 2);
    EXPECT_EQ(results.size(), 2u);
}

// ---- Engine-level n-gram blending tests -----------------------------------

TEST(EngineNgram, TrailingSpaceBlendsPredictedNextTokens) {
    AutocompleteEngine engine;
    engine.insertQuery("hello world", 100);

    // "hello " has a trailing space (word boundary), so the engine returns
    // the trie prefix completion "hello world" AND the n-gram predicted token "world".
    const auto results = engine.getSuggestions("hello ", 5);
    EXPECT_EQ(results, (std::vector<std::string>{"hello world", "world"}));
}

TEST(EngineNgram, NoBlendingWithoutTrailingSpace) {
    AutocompleteEngine engine;
    engine.insertQuery("hello world", 100);

    // Without the trailing space, n-gram does NOT kick in: only trie prefix hits.
    const auto results = engine.getSuggestions("hello", 5);
    EXPECT_EQ(results, (std::vector<std::string>{"hello world"}));
    EXPECT_EQ(std::find(results.begin(), results.end(), "world"), results.end());
}

TEST(EngineNgram, PredictsMultipleNextTokensRankedByFrequency) {
    AutocompleteEngine engine;
    engine.insertQuery("go left", 500);
    engine.insertQuery("go right", 300);
    engine.insertQuery("go home", 100);

    // Query "go " with k=6: exact trie completions fill first, followed by
    // the n-gram next-token predictions in frequency order.
    const auto results = engine.getSuggestions("go ", 6);
    EXPECT_EQ(results, (std::vector<std::string>{
        "go left", "go right", "go home", "left", "right", "home"
    }));
}

TEST(EngineNgram, TrigramTakesPrecedenceInEngine) {
    AutocompleteEngine engine;
    engine.insertQuery("new york city", 1000);
    engine.insertQuery("york times", 9999);

    // Query "new york ":
    // 1. Trie exact prefix -> "new york city"
    // 2. Trigram context ("new", "york") -> "city"
    // 3. Bigram context ("york") -> "times"
    const auto results = engine.getSuggestions("new york ", 5);
    EXPECT_EQ(results, (std::vector<std::string>{"new york city", "city", "times"}));
}

TEST(EngineNgram, KCapIsRespectedAfterBlending) {
    AutocompleteEngine engine;
    engine.insertQuery("hello world", 100);

    // k=1: capped to 1 result (trie hit only)
    EXPECT_EQ(engine.getSuggestions("hello ", 1),
              (std::vector<std::string>{"hello world"}));

    // k=2: capped to 2 results (trie hit + 1 n-gram prediction)
    EXPECT_EQ(engine.getSuggestions("hello ", 2),
              (std::vector<std::string>{"hello world", "world"}));
}

TEST(EngineNgram, LoadCorpusTrainsNgramsAndYieldsBarePredictions) {
    AutocompleteEngine engine;
    engine.loadCorpus(std::string(TRIESTE_DATA_DIR) + "/sample_corpus.txt");

    // sample_corpus.txt has "san francisco 900" and "san diego 450".
    // Querying "san " must return the trie matches AND the bare predicted
    // tokens "francisco" and "diego".
    //
    // k is 6, not 5, for an arithmetic reason: the prefix "san" has FOUR exact
    // trie hits, because "santa clara" carries it too. Four exact plus the two
    // predictions asserted below needs six slots; at k=5 only one prediction
    // could ever fit and this test could not pass however the ranking behaved.
    //
    // Six is also the tightest value that still proves the priority fix. It
    // leaves exactly two free slots, so if fuzzy corrections were still ranked
    // above predictions they would take both ("bank" is one edit from "san")
    // and the assertions below would fail.
    const auto results = engine.getSuggestions("san ", 6);
    EXPECT_NE(std::find(results.begin(), results.end(), "san francisco"), results.end());
    EXPECT_NE(std::find(results.begin(), results.end(), "san diego"), results.end());
    EXPECT_NE(std::find(results.begin(), results.end(), "francisco"), results.end());
    EXPECT_NE(std::find(results.begin(), results.end(), "diego"), results.end());
}

TEST(EngineNgram, DeDuplicatesPredictions) {
    AutocompleteEngine engine;
    engine.insertQuery("hello world", 100);
    engine.insertQuery("hello world", 200);

    const auto results = engine.getSuggestions("hello ", 5);
    EXPECT_EQ(results, (std::vector<std::string>{"hello world", "world"}));
    EXPECT_EQ(std::count(results.begin(), results.end(), "world"), 1);
}

}  // namespace
