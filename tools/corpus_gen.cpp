// Benchmark corpus generator.
//
// Produces data/benchmark_corpus.txt: a realistic, Zipf-weighted English corpus
// for the M5 benchmark suite. It is committed to the repository so benchmark
// runs are reproducible offline and comparable across machines; this tool
// exists so the file can be regenerated and audited rather than taken on faith.
//
// WHY NOT RANDOM STRINGS
//   Random strings share no prefixes. A trie built from them degenerates into a
//   bush of depth-1 branches: prefix search has nothing to share, and the fuzzy
//   walk's subtree pruning -- the entire basis of its sub-O(N) claim -- can
//   never fire, because no subtree is ever large enough to be worth skipping.
//   Benchmarking on random input therefore measures a pathological case and
//   makes the engine look far worse than real usage warrants.
//
// SOURCES (both fetched once; see README / RESULTS.md for the exact commands)
//   words_alpha.txt  dwyl/english-words        370,105 real English words
//   common10k.txt    first20hours/google-10000-english
//                    the 10,000 most frequent English words, IN FREQUENCY ORDER
//
// WHAT THIS BUILDS
//   * 100,000 unique single words drawn from the real dictionary, so prefix
//     clustering is whatever English actually is rather than something modelled.
//   * Frequency ranks: the ~10k common words take the head of the distribution
//     in their real observed order; everything else fills the long tail. The
//     head of the curve is therefore measured English, not an assumption.
//   * Zipfian weights, freq(rank) = C / rank^s with s = 1.0, the standard
//     approximation for word frequency in natural language.
//   * 8,000 multi-word phrases so the n-gram path has real fan-out to traverse.
//
// HONEST LIMITATION: the phrases pair real high-frequency words by a
// deterministic rule; they are not drawn from a real phrase corpus. Their
// STRUCTURE is realistic -- a small set of head words each with many
// continuations, exactly the fan-out an n-gram model sees -- but the specific
// pairings are synthetic. Single-word data is entirely real.
//
// Deterministic by construction: a fixed-seed LCG, no std::random_device, and
// no iteration over unordered containers. Same inputs always give the same file.

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::size_t kTargetWords = 100000;
constexpr std::size_t kTargetPhrases = 8000;
constexpr double kZipfC = 10000000.0;  // weight of the single most frequent term
constexpr double kZipfS = 1.0;         // exponent; ~1.0 is standard for English

/// Fixed-seed LCG. Reproducibility is the point: a benchmark corpus that
/// differs between runs makes today's numbers incomparable with yesterday's.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33;
    }
    std::size_t below(std::size_t bound) { return static_cast<std::size_t>(next() % bound); }

private:
    std::uint64_t state_;
};

std::vector<std::string> readLines(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "corpus_gen: cannot open " << path << "\n";
        std::exit(1);
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

/// Drop the extremes. One- and two-character entries are mostly list artefacts,
/// and words past 18 characters are rare enough that keeping them would skew
/// the length distribution the benchmarks report against.
bool plausibleWord(const std::string& w) {
    if (w.size() < 3 || w.size() > 18) {
        return false;
    }
    for (const char c : w) {
        if (c < 'a' || c > 'z') {
            return false;
        }
    }
    return true;
}

int zipfWeight(std::size_t rank) {  // rank is 1-based
    const double w = kZipfC / std::pow(static_cast<double>(rank), kZipfS);
    return w < 1.0 ? 1 : static_cast<int>(w);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: corpus_gen <words_alpha.txt> <common10k.txt> <output.txt>\n";
        return 1;
    }
    const std::string dictPath = argv[1];
    const std::string commonPath = argv[2];
    const std::string outPath = argv[3];

    // ---- Head of the distribution: real words in real frequency order -------
    const auto common = readLines(commonPath);
    std::vector<std::string> ordered;      // final rank order, most frequent first
    std::unordered_set<std::string> seen;  // lookup only, never iterated

    for (const auto& w : common) {
        if (plausibleWord(w) && seen.insert(w).second) {
            ordered.push_back(w);
        }
    }
    const std::size_t headCount = ordered.size();

    // ---- Long tail: the rest of the dictionary ------------------------------
    //
    // Shuffled before selection rather than taken alphabetically: the first
    // 100k entries of a sorted dictionary are all a- through c- words, which
    // would concentrate the whole corpus into three subtrees and make prefix
    // benchmarks meaningless.
    auto dict = readLines(dictPath);
    std::vector<std::string> tail;
    tail.reserve(dict.size());
    for (auto& w : dict) {
        if (plausibleWord(w) && seen.find(w) == seen.end()) {
            tail.push_back(std::move(w));
        }
    }

    Lcg rng(0xC0FFEE2026ULL);
    for (std::size_t i = tail.size(); i > 1; --i) {  // Fisher-Yates
        std::swap(tail[i - 1], tail[rng.below(i)]);
    }

    for (const auto& w : tail) {
        if (ordered.size() >= kTargetWords) {
            break;
        }
        if (seen.insert(w).second) {
            ordered.push_back(w);
        }
    }

    // ---- Phrases, for the n-gram path ---------------------------------------
    //
    // Heads are drawn from a small pool so each accumulates many continuations,
    // which is the fan-out shape a real n-gram model has to walk. A head pool
    // of 300 over 8000 phrases averages ~27 continuations each.
    std::vector<std::pair<std::string, int>> phrases;
    const std::size_t headPool = std::min<std::size_t>(300, headCount);
    const std::size_t tailPool = std::min<std::size_t>(3000, headCount);
    std::unordered_set<std::string> phraseSeen;

    while (phrases.size() < kTargetPhrases && headPool > 0) {
        const std::string& a = ordered[rng.below(headPool)];
        const std::string& b = ordered[rng.below(tailPool)];
        if (a == b) {
            continue;
        }
        std::string phrase = a + " " + b;
        // A third token on roughly a fifth of them, so the trigram table is
        // exercised too and not just bigrams.
        if (rng.below(5) == 0) {
            const std::string& c = ordered[rng.below(tailPool)];
            if (c != b) {
                phrase += " " + c;
            }
        }
        if (!phraseSeen.insert(phrase).second) {
            continue;
        }
        phrases.emplace_back(std::move(phrase), 0);
    }

    // ---- Emit ----------------------------------------------------------------
    std::ofstream out(outPath);
    if (!out) {
        std::cerr << "corpus_gen: cannot write " << outPath << "\n";
        return 1;
    }

    out << "# trieste benchmark corpus -- generated by tools/corpus_gen.cpp\n"
        << "# " << ordered.size() << " single words + " << phrases.size() << " phrases\n"
        << "# Words: real English (dwyl/english-words). The first " << headCount
        << " are ranked\n"
        << "# by real observed frequency (google-10000-english); the rest form the tail.\n"
        << "# Weights: Zipf, C=" << static_cast<long long>(kZipfC) << " s=" << kZipfS << ".\n"
        << "# Phrases pair real high-frequency words by a deterministic rule; their\n"
        << "# structure is realistic but the specific pairings are synthetic.\n"
        << "# Regenerate: see RESULTS.md.\n";

    for (std::size_t i = 0; i < ordered.size(); ++i) {
        out << ordered[i] << ' ' << zipfWeight(i + 1) << '\n';
    }
    // Phrases are interleaved into the same rank space, offset so they land in
    // the mid-frequency band rather than dominating or vanishing.
    for (std::size_t i = 0; i < phrases.size(); ++i) {
        out << phrases[i].first << ' ' << zipfWeight(50 + i * 3) << '\n';
    }

    std::cout << "wrote " << outPath << ": " << ordered.size() << " words ("
              << headCount << " frequency-ranked from real data) + " << phrases.size()
              << " phrases\n";
    return 0;
}
