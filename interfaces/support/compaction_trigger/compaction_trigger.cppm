module;

#include <cstdint>

export module pi.support.compaction_trigger;

import std;
export import pi.support.agent_token_estimator;
export import pi.support.iso_timestamp;
export import pi.support.overflow_detector;
export import pi.types.compaction_check;
export import pi.types.compaction_decision;

/**
 * Decides whether an assistant response calls for automatic compaction: context overflow (retry
 * the turn once after compacting), a recoverable length stop, or context usage over the
 * configured threshold. Pure: the session performs the chosen action. Port of the decision part
 * of AgentSession._checkCompaction.
 */
export class CompactionTrigger {
public:
    CompactionDecision decide(const CompactionCheck& check) const;

private:
    std::optional<std::int64_t> latestCompactionMs(const std::vector<SessionEntry>& branch) const;
    bool assistantProjected(const CompactionCheck& check) const;
    std::optional<std::size_t> assistantIndex(const CompactionCheck& check) const;
    bool retainedForRecovery(const CompactionCheck& check, std::size_t after) const;
    bool hasEditAfter(const CompactionCheck& check, std::size_t after) const;
    CompactionDecision overflowDecision(const CompactionCheck& check, bool contextOverflow) const;
    bool overflowCase(const CompactionCheck& check, bool& contextOverflow) const;
    std::optional<std::int64_t> thresholdTokens(const CompactionCheck& check,
                                                std::optional<std::int64_t> compactedAtMs) const;

    OverflowDetector m_overflow;
    AgentTokenEstimator m_estimator;
    IsoTimestamp m_iso;
};

std::optional<std::int64_t> CompactionTrigger::latestCompactionMs(
    const std::vector<SessionEntry>& branch) const {
    for (auto it = branch.rbegin(); it != branch.rend(); ++it) {
        if (it->type == "compaction") {
            return m_iso.parse(it->timestamp);
        }
    }
    return std::nullopt;
}

bool CompactionTrigger::assistantProjected(const CompactionCheck& check) const {
    if (!check.assistantEntryId) {
        return true;
    }
    return std::ranges::any_of(check.projection.entries, [&](const ProjectedSessionEntry& entry) {
        return entry.sourceEntry.id == *check.assistantEntryId &&
               std::ranges::any_of(entry.messages, [](const AgentMessage& message) {
                   return std::holds_alternative<AssistantMessage>(message);
               });
    });
}

std::optional<std::size_t> CompactionTrigger::assistantIndex(const CompactionCheck& check) const {
    if (!check.assistantEntryId) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < check.branch.size(); ++i) {
        if (check.branch[i].id == *check.assistantEntryId) {
            return i;
        }
    }
    return std::nullopt;
}

bool CompactionTrigger::hasEditAfter(const CompactionCheck& check, std::size_t after) const {
    for (std::size_t i = after; i < check.branch.size(); ++i) {
        if (check.branch[i].type == "context_edit") {
            return true;
        }
    }
    return false;
}

bool CompactionTrigger::retainedForRecovery(const CompactionCheck& check, std::size_t after) const {
    const Json* latestEdit = nullptr;
    for (std::size_t i = after; i < check.branch.size(); ++i) {
        const SessionEntry& entry = check.branch[i];
        if (entry.type == "compaction") {
            return false;
        }
        if (entry.type == "context_edit" && entry.body.value("targetId", "") == *check.assistantEntryId) {
            latestEdit = &entry.body;
        }
    }
    return latestEdit == nullptr || !latestEdit->contains("replacement") ||
           !(*latestEdit)["replacement"].is_null();
}

bool CompactionTrigger::overflowCase(const CompactionCheck& check, bool& contextOverflow) const {
    const auto index = assistantIndex(check);
    const std::size_t after = index ? *index + 1 : check.branch.size();
    const bool projected = assistantProjected(check);
    const bool usageMatches = projected && !hasEditAfter(check, after);
    const bool retained = !check.assistantEntryId || retainedForRecovery(check, after);
    const bool explicitOverflow = check.assistant.stopReason == StopReason::Error &&
                                  m_overflow.isContextOverflow(check.assistant);
    contextOverflow = check.sameModel &&
                      ((explicitOverflow && retained) ||
                       (usageMatches && m_overflow.isContextOverflow(check.assistant, check.contextWindow)));
    const bool recoverableLength = check.sameModel && projected &&
                                   m_overflow.isRecoverableLength(check.assistant, check.maxOutputTokens);
    return contextOverflow || recoverableLength;
}

CompactionDecision CompactionTrigger::overflowDecision(const CompactionCheck& check,
                                                       bool contextOverflow) const {
    if (check.assistant.stopReason == StopReason::Stop) {
        return {CompactionAction::OverflowNoRetry, std::nullopt};
    }
    if (check.overflowRecoveryAttempted) {
        return {CompactionAction::OverflowRecoveryExhausted,
                contextOverflow ? "Context overflow recovery failed after one compact-and-retry attempt. Try reducing context or switching to a larger-context model."
                                : "Truncated response recovery failed after one compact-and-retry attempt."};
    }
    return {CompactionAction::OverflowRetry, std::nullopt};
}

std::optional<std::int64_t> CompactionTrigger::thresholdTokens(
    const CompactionCheck& check, std::optional<std::int64_t> compactedAtMs) const {
    const bool hasContextEdits = std::ranges::any_of(
        check.projection.entries,
        [](const ProjectedSessionEntry& entry) { return entry.sourceEntry.type == "context_edit"; });
    if (hasContextEdits) {
        return m_estimator.estimateProjected(check.projection, check.branch).tokens;
    }
    const std::int64_t direct = m_estimator.contextTokens(check.assistant.usage);
    if (check.assistant.stopReason != StopReason::Error && direct != 0) {
        return direct;
    }
    const ContextUsageEstimate estimate = m_estimator.estimate(check.messages);
    if (estimate.lastUsageIndex >= 0 && compactedAtMs) {
        // Usage kept from before the compaction describes the old, larger context.
        const auto* usageMessage = std::get_if<AssistantMessage>(
            &check.messages[static_cast<std::size_t>(estimate.lastUsageIndex)]);
        if (usageMessage != nullptr && usageMessage->timestamp <= *compactedAtMs) {
            return std::nullopt;
        }
    }
    return estimate.tokens;
}

CompactionDecision CompactionTrigger::decide(const CompactionCheck& check) const {
    if (!check.settings.enabled) {
        return {};
    }
    if (check.skipAbortedCheck && check.assistant.stopReason == StopReason::Aborted) {
        return {};
    }
    const auto compactedAt = latestCompactionMs(check.branch);
    // A response older than the latest compaction must not retrigger it.
    if (compactedAt && check.assistant.timestamp <= *compactedAt) {
        return {};
    }
    bool contextOverflow = false;
    if (overflowCase(check, contextOverflow)) {
        return overflowDecision(check, contextOverflow);
    }
    const auto tokens = thresholdTokens(check, compactedAt);
    if (tokens && m_estimator.shouldCompact(*tokens, check.contextWindow, check.settings)) {
        return {CompactionAction::Threshold, std::nullopt};
    }
    return {};
}
