export module pi.types.tree_preparation;

import std;
export import pi.types.session_entry;

/** Data of a tree navigation about to happen, offered to the `session_before_tree` plugin event. */
export struct TreePreparation {
    std::string targetId;
    std::optional<std::string> oldLeafId;
    std::optional<std::string> commonAncestorId;
    std::vector<SessionEntry> entriesToSummarize;
    bool userWantsSummary = false;
    std::optional<std::string> customInstructions;
    bool replaceInstructions = false;
    std::optional<std::string> label;
};
