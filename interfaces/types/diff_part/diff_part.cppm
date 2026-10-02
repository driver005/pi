export module pi.types.diff_part;

import std;

/** One run of a line diff: lines (without newline) that are common, added or removed. */
export struct DiffPart {
    bool added = false;
    bool removed = false;
    std::vector<std::string> lines;
};
