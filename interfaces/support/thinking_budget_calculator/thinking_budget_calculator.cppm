module;

#include <nlohmann/json.hpp>

export module pi.support.thinking_budget_calculator;

import std;
export import pi.support.token_estimator;
export import pi.types.json;
export import pi.types.model;
export import pi.types.thinking_budget_plan;
export import pi.types.thinking_level;
export import pi.types.transcript_context;

/** Token ceilings and thinking budgets. Port of api/simple-options.ts. */
export class ThinkingBudgetCalculator {
public:
    /** Tokens always left for the answer when a thinking budget shares the response ceiling. */
    static constexpr std::int64_t MinAnswerTokens = 1024;

    /** Caps maxTokens so the prompt plus answer fits the context window. */
    std::int64_t clampMaxTokensToContext(const Model& model, const TranscriptContext& context,
                                         std::int64_t maxTokens) const;

    /** Budget for a level; custom is {"minimal":n,...}. xhigh and max use the high budget. */
    std::int64_t budgetForLevel(ThinkingLevel level, const Json& custom) const;

    /** Fits a thinking budget inside the response ceiling. No cap means the model cap. */
    ThinkingBudgetPlan adjustMaxTokens(std::optional<std::int64_t> baseMaxTokens,
                                       std::int64_t modelMaxTokens, ThinkingLevel level,
                                       const Json& custom) const;

private:
    std::int64_t defaultBudget(ThinkingLevel level) const;
    std::string keyFor(ThinkingLevel level) const;

    TokenEstimator m_estimator;
};

std::string ThinkingBudgetCalculator::keyFor(ThinkingLevel level) const {
    switch (level) {
        case ThinkingLevel::Off:
        case ThinkingLevel::Minimal: return "minimal";
        case ThinkingLevel::Low: return "low";
        case ThinkingLevel::Medium: return "medium";
        case ThinkingLevel::High:
        case ThinkingLevel::XHigh:
        case ThinkingLevel::Max: return "high";
    }
    return "high";
}

std::int64_t ThinkingBudgetCalculator::defaultBudget(ThinkingLevel level) const {
    const std::string key = keyFor(level);
    if (key == "minimal") {
        return 1024;
    }
    if (key == "low") {
        return 2048;
    }
    if (key == "medium") {
        return 8192;
    }
    return 16384;
}

std::int64_t ThinkingBudgetCalculator::clampMaxTokensToContext(
    const Model& model, const TranscriptContext& context, std::int64_t maxTokens) const {
    if (model.contextWindow <= 0) {
        return std::max<std::int64_t>(1, maxTokens);
    }
    const std::int64_t available =
        model.contextWindow - m_estimator.estimate(context).tokens - 4096;
    return std::min(maxTokens, std::max<std::int64_t>(1, available));
}

std::int64_t ThinkingBudgetCalculator::budgetForLevel(ThinkingLevel level, const Json& custom) const {
    const std::string key = keyFor(level);
    if (custom.is_object() && custom.contains(key) && custom[key].is_number()) {
        return custom[key].get<std::int64_t>();
    }
    return defaultBudget(level);
}

ThinkingBudgetPlan ThinkingBudgetCalculator::adjustMaxTokens(
    std::optional<std::int64_t> baseMaxTokens, std::int64_t modelMaxTokens, ThinkingLevel level,
    const Json& custom) const {
    ThinkingBudgetPlan plan;
    plan.thinkingBudget = budgetForLevel(level, custom);
    plan.maxTokens = baseMaxTokens ? std::min(*baseMaxTokens + plan.thinkingBudget, modelMaxTokens)
                                   : modelMaxTokens;
    if (plan.maxTokens <= plan.thinkingBudget) {
        plan.thinkingBudget =
            std::min(plan.thinkingBudget, std::max<std::int64_t>(0, plan.maxTokens - MinAnswerTokens));
    }
    return plan;
}
