module;

#include <cstdint>

export module pi.types.thinking_budget_plan;

import std;

/** Response ceiling and the part of it granted to thinking. */
export struct ThinkingBudgetPlan {
    std::int64_t maxTokens = 0;
    std::int64_t thinkingBudget = 0;
};
