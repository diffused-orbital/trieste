// Tiny interactive demo for the engine. Not part of the library.
//
//   trieste_cli [corpus.txt] [k]
//
// With no corpus path it falls back to the spec's four-word example. Type a
// prefix, press enter, get the Top-K. Blank line or EOF (Ctrl-D / Ctrl-Z) exits.

#include <exception>
#include <iostream>
#include <string>

#include "trieste/autocomplete_engine.hpp"

int main(int argc, char** argv) {
    trieste::AutocompleteEngine engine;
    int k = 5;

    try {
        if (argc > 1) {
            engine.loadCorpus(argv[1]);
            std::cout << "loaded " << argv[1] << '\n';
        } else {
            engine.insertQuery("apple", 500);
            engine.insertQuery("apply", 300);
            engine.insertQuery("application", 1000);
            engine.insertQuery("apricot", 150);
            std::cout << "no corpus given; using the built-in example\n";
        }
        if (argc > 2) {
            k = std::stoi(argv[2]);
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    std::cout << engine.termCount() << " terms across " << engine.nodeCount()
              << " nodes. Type a prefix (blank line to quit).\n";

    std::string prefix;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, prefix) || prefix.empty()) {
            break;
        }
        const auto suggestions = engine.getScoredSuggestions(prefix, k);
        if (suggestions.empty()) {
            std::cout << "  (no matches)\n";
            continue;
        }
        for (const auto& entry : suggestions) {
            std::cout << "  " << entry.term << "  [" << entry.frequency << "]\n";
        }
    }
    return 0;
}
