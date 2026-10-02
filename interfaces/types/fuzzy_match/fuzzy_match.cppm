module;

#include <cstddef>

export module pi.types.fuzzy_match;

import std;

/** Result of locating text: exact first, then in whitespace/quote/dash-normalized space. */
export struct FuzzyMatch {
    bool found = false;
    std::size_t index = 0;
    std::size_t matchLength = 0;
    bool usedFuzzyMatch = false;
    /** Content the offsets refer to: the original when exact, the normalized text when fuzzy. */
    std::string contentForReplacement;
};
