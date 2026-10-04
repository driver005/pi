module;

#include <cstdint>

export module pi.types.anthropic_thinking_plan;

import std;

/** Resolved thinking parameters for one Anthropic request. */
export struct AnthropicThinkingPlan {
    bool enabled = false;
    /** "low" | "medium" | "high" | "xhigh" | "max" for adaptive thinking models. */
    std::optional<std::string> effort;
    /** Budget for budget-based thinking models. */
    std::optional<std::int64_t> budgetTokens;
    /** Response ceiling after fitting the budget. */
    std::int64_t maxTokens = 0;
};
