module;

#include <cstdint>

export module pi.support.context_usage_calculator;

import std;
export import pi.support.agent_token_estimator;
export import pi.types.context_usage;
export import pi.types.session_entry;
export import pi.types.session_projection;

/**
 * Current context size against a model's window. After a compaction the last usage describes the
 * old, larger context, so the size is unknown until a response arrives that follows the
 * compaction. Port of AgentSession.getContextUsage.
 */
export class ContextUsageCalculator {
public:
    /** nullopt when the window is unknown (zero). */
    std::optional<ContextUsage> calculate(std::int64_t contextWindow, const SessionProjection& projection,
                                          const std::vector<SessionEntry>& branch) const;

private:
    bool hasUsageAfterCompaction(const SessionProjection& projection,
                                 const std::vector<SessionEntry>& branch, std::size_t compaction) const;
    std::optional<std::size_t> latestCompaction(const std::vector<SessionEntry>& branch) const;

    AgentTokenEstimator m_estimator;
};

std::optional<std::size_t> ContextUsageCalculator::latestCompaction(
    const std::vector<SessionEntry>& branch) const {
    for (std::size_t i = branch.size(); i-- > 0;) {
        if (branch[i].type == "compaction") {
            return i;
        }
    }
    return std::nullopt;
}

bool ContextUsageCalculator::hasUsageAfterCompaction(const SessionProjection& projection,
                                                     const std::vector<SessionEntry>& branch,
                                                     std::size_t compaction) const {
    std::set<std::string> withUsage;
    for (const auto& entry : projection.entries) {
        if (std::ranges::any_of(entry.messages, [&](const AgentMessage& message) {
                return m_estimator.assistantUsage(message).has_value();
            })) {
            withUsage.insert(entry.sourceEntry.id);
        }
    }
    for (std::size_t i = compaction + 1; i < branch.size(); ++i) {
        if (withUsage.contains(branch[i].id)) {
            return true;
        }
    }
    return false;
}

std::optional<ContextUsage> ContextUsageCalculator::calculate(
    std::int64_t contextWindow, const SessionProjection& projection,
    const std::vector<SessionEntry>& branch) const {
    if (contextWindow <= 0) {
        return std::nullopt;
    }
    ContextUsage usage;
    usage.contextWindow = contextWindow;
    const auto compaction = latestCompaction(branch);
    if (compaction && !hasUsageAfterCompaction(projection, branch, *compaction)) {
        return usage;
    }
    const std::int64_t tokens = m_estimator.estimateProjected(projection, branch).tokens;
    usage.tokens = tokens;
    usage.percent = static_cast<double>(tokens) / static_cast<double>(contextWindow) * 100.0;
    return usage;
}
