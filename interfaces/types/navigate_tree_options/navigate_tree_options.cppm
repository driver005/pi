export module pi.types.navigate_tree_options;

import std;

export struct NavigateTreeOptions {
    /** Summarize the branch being left. */
    bool summarize = false;
    std::optional<std::string> customInstructions;
    /** customInstructions replaces the default summarization prompt. */
    bool replaceInstructions = false;
    /** Label attached to the summary entry (or to the target when not summarizing). */
    std::optional<std::string> label;
};
