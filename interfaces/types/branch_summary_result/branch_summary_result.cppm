export module pi.types.branch_summary_result;

import std;
export import pi.types.usage;

/** Branch summary outcome: a summary, or aborted, or an error message. */
export struct BranchSummaryResult {
    std::optional<std::string> summary;
    std::optional<Usage> usage;
    std::optional<std::vector<std::string>> readFiles;
    std::optional<std::vector<std::string>> modifiedFiles;
    bool aborted = false;
    std::optional<std::string> error;
};
