module;

#include <cstddef>

export module pi.types.matched_edit;

import std;

/** An edit located in the content (byte offsets) and ready to be applied. */
export struct MatchedEdit {
    std::size_t editIndex = 0;
    std::size_t matchIndex = 0;
    std::size_t matchLength = 0;
    std::string newText;
};
