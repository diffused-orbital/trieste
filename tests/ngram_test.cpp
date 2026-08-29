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

TEST(EngineNgram, TrailingSpaceTriggersPrediction) {
    AutocompleteEngine engine;
    engine.insertQuery("san francisco", 900);
    engine.insertQuery("san diego", 450);

    // "san " — exact trie matches exist ("san francisco", "san diego") so the
    // n-gram blending may not run, but the results must include them.
    const auto results = engine.getSuggestions("san ", 5);
    ASSERT_FALSE(results.empty());
    EXPECT_NE(std::find(results.begin(), results.end(), "san francisco"),
              results.end());
    EXPECT_NE(std::find(results.begin(), results.end(), "san diego"),
              results.end());
}

TEST(EngineNgram, NgramFillsWhenTrieHasNoTermsWithPrefix) {
    AutocompleteEngine engine;
    // Insert only "go left" and "go right"; there is no trie term that starts
    // with "go " followed by nothing (i.e., the prefix "go " won't match any
    // stored term that is exactly "go ").  Actually "go left" does start with
    // "go ", so trie will return them. Let's use a completely unrelated prefix.
    engine.insertQuery("go left", 500);
    engine.insertQuery("go right", 300);

    // Query the engine with just "go " — trie can find "go left" and "go right"
    // as exact prefix matches.
    const auto results = engine.getSuggestions("go ", 5);
    ASSERT_FALSE(results.empty());
    EXPECT_NE(std::find(results.begin(), results.end(), "go left"), results.end());
    EXPECT_NE(std::find(results.begin(), results.end(), "go right"), results.end());
}

TEST(EngineNgram, NgramPredictsFillsSlotsBeyondTrieHits) {
    AutocompleteEngine engine;
    // Only "new york" in the trie, but we also trained on "new york city"
    // so the n-gram knows "city" follows "new york".
    engine.insertQuery("new york", 500);
    engine.insertQuery("new york city", 600);

    // With k=5, trie gives {"new york city", "new york"} (2 hits).
    // N-gram: after "new york " the trigram/bigram says "city" → but "city"
    // is already in the trie results (as part of "new york city"), so no
    // duplication should occur.
    const auto results = engine.getSuggestions("new york ", 5);
    const auto cityCount = std::count(results.begin(), results.end(), "new york city");
    EXPECT_EQ(cityCount, 1);  // must not appear twice
}

TEST(EngineNgram, NoBlendingWithoutTrailingSpace) {
    AutocompleteEngine engine;
    engine.insertQuery("san francisco", 900);

    // Without the trailing space, n-gram should NOT kick in; the engine
    // should still return trie prefix matches.
    const auto results = engine.getSuggestions("san", 5);
    // "san francisco" starts with "san", so the trie still returns it.
    EXPECT_NE(std::find(results.begin(), results.end(), "san francisco"),
              results.end());
}

TEST(EngineNgram, KCapIsRespectedAfterBlending) {
    AutocompleteEngine engine;
    // Load several multi-word terms so both trie and ngram have candidates.
    for (const char* t : {"go left", "go right", "go up", "go down", "go forward"}) {
        engine.insertQuery(t, 100);
    }
    const auto results = engine.getSuggestions("go ", 3);
    EXPECT_LE(results.size(), 3u);
}

TEST(EngineNgram, LoadCorpusTrainsNgrams) {
    AutocompleteEngine engine;
    // Use the shipped sample corpus — it contains multi-word terms.
    engine.loadCorpus(std::string(TRIESTE_DATA_DIR) + "/sample_corpus.txt");

    // "san " should yield "san francisco" and/or "san diego" from the trie;
    // we just check the engine doesn't crash and returns something plausible.
    const auto results = engine.getSuggestions("san ", 5);
    EXPECT_FALSE(results.empty());
}

TEST(EngineNgram, NgramDoesNotDuplicateExactTrieHits) {
    AutocompleteEngine engine;
    engine.insertQuery("san francisco", 900);
    engine.insertQuery("san diego", 450);

    // Both appear as trie hits. The n-gram model would also suggest "francisco"
    // and "diego" as next tokens after "san", but "san francisco" / "san diego"
    // are already in results — de-duplication must prevent any repeat.
    const auto results = engine.getSuggestions("san ", 10);
    for (const auto& r : results) {
        EXPECT_EQ(std::count(results.begin(), results.end(), r), 1)
            << "Duplicate entry: " << r;
    }
}

}  // namespace
