export module pi.types.applied_edits;

import std;

/** Content before and after applying edits (both LF-normalized). */
export struct AppliedEdits {
    std::string baseContent;
    std::string newContent;
};
