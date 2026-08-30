#include "trieste/trie.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using trieste::ScoredTerm;
using trieste::Trie;

std::vector<std::string> termsOf(const std::vector<ScoredTerm>& scored) {
    std::vector<std::string> terms;
    terms.reserve(scored.size());
    for (const auto& entry : scored) {
        terms.push_back(entry.term);
    }
    return terms;
}

Trie specCorpus() {
    Trie trie;
    trie.insert("apple", 500);
    trie.insert("apply", 300);
    trie.insert("application", 1000);
    trie.insert("apricot", 150);
    return trie;
}

TEST(TrieInsert, StartsEmpty) {
    const Trie trie;
    EXPECT_TRUE(trie.empty());
    EXPECT_EQ(trie.termCount(), 0u);
    EXPECT_EQ(trie.nodeCount(), 1u);  // just the root
}

TEST(TrieInsert, RecordsTermsAndFrequencies) {
    const Trie trie = specCorpus();
    EXPECT_FALSE(trie.empty());
    EXPECT_EQ(trie.termCount(), 4u);
    EXPECT_EQ(trie.frequencyOf("application"), 1000);
    EXPECT_TRUE(trie.contains("apple"));
    EXPECT_FALSE(trie.contains("apricots"));
}

TEST(TrieInsert, PrefixOfATermIsNotItselfATerm) {
    const Trie trie = specCorpus();
    EXPECT_EQ(trie.frequencyOf("appl"), 0);
    EXPECT_FALSE(trie.contains("appl"));
}

TEST(TrieInsert, AccumulatesWeightOnRepeatedInsert) {
    Trie trie;
    trie.insert("apple", 500);
    trie.insert("apple", 25);
    EXPECT_EQ(trie.frequencyOf("apple"), 525);
    EXPECT_EQ(trie.termCount(), 1u);  // still one distinct term
}

TEST(TrieInsert, SharesNodesAcrossCommonPrefixes) {
    Trie trie;
    trie.insert("app", 1);
    const std::size_t afterFirst = trie.nodeCount();
    trie.insert("apply", 1);
    // "appl" + "y" -- only two new nodes, the "app" path is reused.
    EXPECT_EQ(trie.nodeCount(), afterFirst + 2);
}

TEST(TrieInsert, IgnoresEmptyWordsAndNonPositiveWeights) {
    Trie trie;
    trie.insert("", 100);
    trie.insert("apple", 0);
    trie.insert("apple", -5);
    EXPECT_TRUE(trie.empty());
    EXPECT_EQ(trie.termCount(), 0u);
}

TEST(TrieTopK, RanksByFrequencyDescending) {
    const Trie trie = specCorpus();
    EXPECT_EQ(termsOf(trie.topKWithPrefix("appl", 3)),
              (std::vector<std::string>{"application", "apple", "apply"}));
}

TEST(TrieTopK, HonoursTheBound) {
    const Trie trie = specCorpus();
    // The whole point of the bounded heap: k caps the output even though the
    // subtree under "appl" holds three terms.
    EXPECT_EQ(termsOf(trie.topKWithPrefix("appl", 2)),
              (std::vector<std::string>{"application", "apple"}));
}

TEST(TrieTopK, ReturnsFewerThanKWhenTheSubtreeIsSmall) {
    const Trie trie = specCorpus();
    const auto results = trie.topKWithPrefix("apr", 5);
    EXPECT_EQ(termsOf(results), (std::vector<std::string>{"apricot"}));
}

TEST(TrieTopK, KeepsTheFrequencyThatEarnedEachSlot) {
    const Trie trie = specCorpus();
    const auto results = trie.topKWithPrefix("appl", 2);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], (ScoredTerm{"application", 1000}));
    EXPECT_EQ(results[1], (ScoredTerm{"apple", 500}));
}

TEST(TrieTopK, BreaksTiesLexicographically) {
    Trie trie;
    trie.insert("beta", 10);
    trie.insert("alpha", 10);
    trie.insert("gamma", 10);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("", 2)),
              (std::vector<std::string>{"alpha", "beta"}));
}

TEST(TrieTopK, EmptyPrefixRanksTheWholeDictionary) {
    const Trie trie = specCorpus();
    EXPECT_EQ(termsOf(trie.topKWithPrefix("", 2)),
              (std::vector<std::string>{"application", "apple"}));
}

TEST(TrieTopK, ExactTermIsItsOwnPrefix) {
    const Trie trie = specCorpus();
    EXPECT_EQ(termsOf(trie.topKWithPrefix("apple", 5)),
              (std::vector<std::string>{"apple"}));
}

TEST(TrieTopK, UnknownPrefixYieldsNothing) {
    const Trie trie = specCorpus();
    EXPECT_TRUE(trie.topKWithPrefix("zebra", 5).empty());
    EXPECT_TRUE(trie.topKWithPrefix("applesauce", 5).empty());
}

TEST(TrieTopK, ZeroKYieldsNothing) {
    const Trie trie = specCorpus();
    EXPECT_TRUE(trie.topKWithPrefix("appl", 0).empty());
}

TEST(TrieTopK, EvictsCorrectlyWhenTheWinnerArrivesLast) {
    // Guards the eviction branch: insert in ascending weight order so every new
    // term has to displace the incumbent at the top of the min-heap.
    Trie trie;
    trie.insert("ca", 1);
    trie.insert("cb", 2);
    trie.insert("cc", 3);
    trie.insert("cd", 4);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("c", 2)), (std::vector<std::string>{"cd", "cc"}));
}

// ---- M6: subtreeMax maintenance -----------------------------------------
//
// topKWithPrefix now descends best-first using each node's cached subtree
// maximum instead of walking the whole subtree. These pin the cases where a
// stale or wrongly-maintained bound would silently reorder results.

TEST(TrieTopK, RankingFollowsRepeatedInsertsThatChangeTheOrder) {
    Trie trie;
    trie.insert("alpha", 100);
    trie.insert("alps", 50);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("al", 2)),
              (std::vector<std::string>{"alpha", "alps"}));

    // Push the runner-up past the leader. The cached bounds on "a" and "al"
    // must rise with it, or the winner never surfaces.
    trie.insert("alps", 500);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("al", 2)),
              (std::vector<std::string>{"alps", "alpha"}));
}

TEST(TrieTopK, NodeWhoseOwnTermRanksBelowItsDescendant) {
    // "app" is a term in its own right but is outranked by something beneath
    // it. The bound on "app" is 1000 (from "apple"), not its own 500, so the
    // walk must not mistake the node for the answer when it pops it.
    Trie trie;
    trie.insert("app", 500);
    trie.insert("apple", 1000);
    trie.insert("apply", 700);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("app", 3)),
              (std::vector<std::string>{"apple", "apply", "app"}));
}

TEST(TrieTopK, PrefixThatIsAlsoATermSortsAheadOfItsExtensionsOnATie) {
    // Equal frequencies: a prefix must precede anything extending it, which is
    // what keeps the best-first order identical to a full sort.
    Trie trie;
    trie.insert("go", 10);
    trie.insert("goal", 10);
    trie.insert("going", 10);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("go", 3)),
              (std::vector<std::string>{"go", "goal", "going"}));
}

TEST(TrieTopK, EmptyPrefixStaysCorrectAsInsertsArrive) {
    // The empty prefix reads the root's bound, the one most likely to be left
    // stale by a faulty update.
    Trie trie;
    trie.insert("aaa", 1);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("", 1)), (std::vector<std::string>{"aaa"}));
    trie.insert("zzz", 2);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("", 1)), (std::vector<std::string>{"zzz"}));
    trie.insert("mmm", 3);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("", 2)),
              (std::vector<std::string>{"mmm", "zzz"}));
}

TEST(TrieTopK, DeepChainDoesNotLoseTheAnswer) {
    // One long single-child chain: every node on it carries the same bound, so
    // the walk must keep descending rather than stopping at the first node
    // whose bound equals the target.
    Trie trie;
    trie.insert("abcdefghijklmnop", 42);
    trie.insert("abcdefghijklmnoq", 7);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("abc", 2)),
              (std::vector<std::string>{"abcdefghijklmnop", "abcdefghijklmnoq"}));
}

TEST(TrieMisc, ClearResetsEverything) {
    Trie trie = specCorpus();
    trie.clear();
    EXPECT_TRUE(trie.empty());
    EXPECT_EQ(trie.termCount(), 0u);
    EXPECT_EQ(trie.nodeCount(), 1u);
    EXPECT_TRUE(trie.topKWithPrefix("appl", 5).empty());
}

TEST(TrieMisc, HandlesNonAlphabeticCharacters) {
    // The sorted-vector node accepts any char, which is what M3's multi-word
    // keys will need -- a fixed a-z array could not do this.
    Trie trie;
    trie.insert("san francisco", 900);
    trie.insert("san diego", 450);
    EXPECT_EQ(termsOf(trie.topKWithPrefix("san ", 2)),
              (std::vector<std::string>{"san francisco", "san diego"}));
}

}  // namespace
