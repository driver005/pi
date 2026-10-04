module;

#include <cstdint>

export module pi.types.before_tree_outcome;

import std;
export import pi.types.json;
export import pi.types.usage;

/** What the `session_before_tree` plugin event decided: cancel, a summary of its own, or changed summarization options. */
export struct BeforeTreeOutcome {
    bool cancel = false;
    std::optional<std::string> summary;
    Json details;
    std::optional<Usage> usage;
    std::optional<std::string> customInstructions;
    std::optional<bool> replaceInstructions;
    std::optional<std::string> label;
};
