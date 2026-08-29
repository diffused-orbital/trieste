#include "trieste/autocomplete_engine.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using trieste::AutocompleteEngine;

void loadSpecCorpus(AutocompleteEngine& engine) {
    engine.insertQuery("apple", 500);
    engine.insertQuery("apply", 300);
    engine.insertQuery("application", 1000);
    engine.insertQuery("apricot", 150);
}

/// Writes a corpus into a uniquely named temp file and removes it afterwards,
/// so the tests never depend on -- or leave behind -- repo state.
class TempCorpus {
public:
    explicit TempCorpus(const std::string& contents) {
        path_ = std::filesystem::temp_directory_path() /
                ("trieste_test_" + std::to_string(++counter_) + ".txt");
        std::ofstream out(path_);
        out << contents;
    }
    ~TempCorpus() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempCorpus(const TempCorpus&) = delete;
    TempCorpus& operator=(const TempCorpus&) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// ---- The spec's worked example ------------------------------------------

TEST(SpecExample, ApplReturnsApplicationThenApple) {
    AutocompleteEngine engine;
    loadSpecCorpus(engine);
    // Corpus: apple 500, apply 300, application 1000, apricot 150
    EXPECT_EQ(engine.getSuggestions("appl", /*k=*/2, /*maxEditDistance=*/1),
              (std::vector<std::string>{"application", "apple"}));
}

TEST(SpecExample, DefaultArgumentsMatchTheDeclaredSignature) {
    AutocompleteEngine engine;
    loadSpecCorpus(engine);
    // k defaults to 5, maxEditDistance to 2.
    EXPECT_EQ(engine.getSuggestions("ap"),
              (std::vector<std::string>{"application", "apple", "apply", "apricot"}));
}

// M2 will make this pass. It is disabled rather than deleted so the target
// stays visible in the test list: `ctest` prints it as skipped.
TEST(SpecExample, DISABLED_TypoCorrectionArrivesInM2) {
    AutocompleteEngine engine;
    engine.insertQuery("hello", 900);
    engine.insertQuery("help", 850);
    EXPECT_EQ(engine.getSuggestions("heloo", /*k=*/2, /*maxEditDistance=*/1),
              (std::vector<std::string>{"hello", "help"}));
}

// ---- Facade behaviour ----------------------------------------------------

TEST(Engine, StartsEmpty) {
    const AutocompleteEngine engine;
    EXPECT_EQ(engine.termCount(), 0u);
    EXPECT_TRUE(engine.getSuggestions("anything").empty());
}

TEST(Engine, InsertQueryAccumulatesWeight) {
    AutocompleteEngine engine;
    engine.insertQuery("apply", 300);
    engine.insertQuery("apple", 100);
    engine.insertQuery("apple", 100);
    engine.insertQuery("apple", 150);  // 350 total -- now outranks "apply"
    EXPECT_EQ(engine.getSuggestions("appl", 1), (std::vector<std::string>{"apple"}));
    EXPECT_EQ(engine.termCount(), 2u);
}

TEST(Engine, InsertQueryDefaultsToWeightOne) {
    AutocompleteEngine engine;
    engine.insertQuery("apple");
    engine.insertQuery("apple");
    const auto scored = engine.getScoredSuggestions("app", 1);
    ASSERT_EQ(scored.size(), 1u);
    EXPECT_EQ(scored[0].frequency, 2);
}

TEST(Engine, MatchingIsCaseInsensitiveAndTrimmed) {
    AutocompleteEngine engine;
    engine.insertQuery("  Apple  ", 500);
    EXPECT_EQ(engine.termCount(), 1u);
    EXPECT_EQ(engine.getSuggestions("APPL", 1), (std::vector<std::string>{"apple"}));
    EXPECT_EQ(engine.getSuggestions(" aP ", 1), (std::vector<std::string>{"apple"}));
}

TEST(Engine, NormalizeCollapsesInternalWhitespace) {
    EXPECT_EQ(AutocompleteEngine::normalize("  San   FRANCISCO \t"), "san francisco");
    EXPECT_EQ(AutocompleteEngine::normalize("   "), "");
    EXPECT_EQ(AutocompleteEngine::normalize(""), "");
}

TEST(Engine, NonPositiveKYieldsNothing) {
    AutocompleteEngine engine;
    loadSpecCorpus(engine);
    EXPECT_TRUE(engine.getSuggestions("appl", 0).empty());
    EXPECT_TRUE(engine.getSuggestions("appl", -3).empty());
}

TEST(Engine, ReturnsFewerThanKWhenExactMatchesRunOut) {
    // The M2 trigger condition: fewer than k exact matches. For now the engine
    // simply returns what it has instead of correcting.
    AutocompleteEngine engine;
    loadSpecCorpus(engine);
    EXPECT_EQ(engine.getSuggestions("apric", 5), (std::vector<std::string>{"apricot"}));
    EXPECT_TRUE(engine.getSuggestions("xyz", 5).empty());
}

// ---- Corpus loading ------------------------------------------------------

TEST(LoadCorpus, ParsesTermAndWeightPairs) {
    const TempCorpus corpus("apple 500\napply 300\napplication 1000\napricot 150\n");
    AutocompleteEngine engine;
    engine.loadCorpus(corpus.path());
    EXPECT_EQ(engine.termCount(), 4u);
    EXPECT_EQ(engine.getSuggestions("appl", 2),
              (std::vector<std::string>{"application", "apple"}));
}

TEST(LoadCorpus, SkipsBlankLinesAndComments) {
    const TempCorpus corpus("# a comment\n\n   \napple 500\n\t# indented comment\napply 300\n");
    AutocompleteEngine engine;
    engine.loadCorpus(corpus.path());
    EXPECT_EQ(engine.termCount(), 2u);
}

TEST(LoadCorpus, DefaultsMissingWeightToOne) {
    const TempCorpus corpus("apple\napply 300\n");
    AutocompleteEngine engine;
    engine.loadCorpus(corpus.path());
    const auto scored = engine.getScoredSuggestions("apple", 1);
    ASSERT_EQ(scored.size(), 1u);
    EXPECT_EQ(scored[0].frequency, 1);
}

TEST(LoadCorpus, KeepsMultiWordTermsIntact) {
    // Splitting on the LAST whitespace is what makes this work.
    const TempCorpus corpus("san francisco 900\nsan diego 450\nnew york\n");
    AutocompleteEngine engine;
    engine.loadCorpus(corpus.path());
    EXPECT_EQ(engine.termCount(), 3u);
    EXPECT_EQ(engine.getSuggestions("san ", 2),
              (std::vector<std::string>{"san francisco", "san diego"}));
    EXPECT_EQ(engine.getSuggestions("new", 1), (std::vector<std::string>{"new york"}));
}

TEST(LoadCorpus, ToleratesCarriageReturns) {
    const TempCorpus corpus("apple 500\r\napply 300\r\n");
    AutocompleteEngine engine;
    engine.loadCorpus(corpus.path());
    EXPECT_EQ(engine.termCount(), 2u);
    EXPECT_EQ(engine.getSuggestions("appl", 1), (std::vector<std::string>{"apple"}));
}

TEST(LoadCorpus, MergesRatherThanReplaces) {
    const TempCorpus first("apple 500\n");
    const TempCorpus second("apple 100\napply 300\n");
    AutocompleteEngine engine;
    engine.loadCorpus(first.path());
    engine.loadCorpus(second.path());
    EXPECT_EQ(engine.termCount(), 2u);
    const auto scored = engine.getScoredSuggestions("apple", 1);
    ASSERT_EQ(scored.size(), 1u);
    EXPECT_EQ(scored[0].frequency, 600);
}

TEST(LoadCorpus, ThrowsOnAMissingFile) {
    AutocompleteEngine engine;
    EXPECT_THROW(engine.loadCorpus("no/such/corpus.txt"), std::runtime_error);
}

TEST(LoadCorpus, ReadsTheShippedSampleCorpus) {
    AutocompleteEngine engine;
    engine.loadCorpus(std::string(TRIESTE_DATA_DIR) + "/sample_corpus.txt");
    EXPECT_GT(engine.termCount(), 20u);
    EXPECT_EQ(engine.getSuggestions("appl", 2),
              (std::vector<std::string>{"application", "apple"}));
    EXPECT_EQ(engine.getSuggestions("hel", 2), (std::vector<std::string>{"hello", "help"}));
}

}  // namespace
