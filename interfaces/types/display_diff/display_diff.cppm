export module pi.types.display_diff;

import std;

/** Line-numbered, context-trimmed diff for display plus the first changed line in the new file. */
export struct DisplayDiff {
    std::string diff;
    std::optional<int> firstChangedLine;
};
