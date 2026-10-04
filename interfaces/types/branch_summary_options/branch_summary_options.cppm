module;

#include <cstdint>

export module pi.types.branch_summary_options;

import std;
export import pi.types.summarization_options;

/** Branch summary request: how to reach the model plus instruction and budget settings. */
export struct BranchSummaryOptions {
    SummarizationOptions summarization;
    std::optional<std::string> customInstructions;
    /** customInstructions replaces the default prompt instead of being appended. */
    bool replaceInstructions = false;
    /** Tokens reserved when selecting branch history. */
    std::int64_t reserveTokens = 16384;
};
