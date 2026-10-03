module;

#include <cstdint>

export module pi.support.cut_point_finder;

import std;
export import pi.support.agent_message_codec;
export import pi.support.agent_token_estimator;
export import pi.support.session_projector;
export import pi.types.cut_point_result;
export import pi.types.projected_session_entry;

/**
 * Chooses where compaction cuts the context: walks back from the newest entry until about
 * keepRecentTokens are kept, then moves to the closest message that may start the kept range
 * (never a tool result). Port of findProjectedCutPoint in compaction/compaction.ts.
 */
export class CutPointFinder {
public:
    CutPointResult find(const std::vector<ProjectedSessionEntry>& entries, std::size_t start,
                        std::size_t end, std::int64_t keepRecentTokens) const;

private:
    bool isCutPointRole(const std::string& role) const;
    bool isTurnStartRole(const std::string& role) const;
    bool hasCutPointMessage(const ProjectedSessionEntry& entry) const;
    bool startsTurn(const ProjectedSessionEntry& entry) const;
    int turnStartBefore(const std::vector<ProjectedSessionEntry>& entries, std::size_t index,
                        std::size_t start) const;
    std::vector<std::size_t> cutPoints(const std::vector<ProjectedSessionEntry>& entries,
                                       std::size_t start, std::size_t end) const;
    std::optional<std::size_t> budgetCut(const std::vector<ProjectedSessionEntry>& entries,
                                         const std::vector<std::size_t>& points, std::size_t start,
                                         std::size_t end, std::int64_t keepRecentTokens) const;
    bool isVisible(const ProjectedSessionEntry& entry) const;
    bool isOmitted(const ProjectedSessionEntry& entry) const;
    bool isAssistantMessageEntry(const ProjectedSessionEntry& entry) const;
    bool isRecoveryOmissionSuffix(const std::vector<ProjectedSessionEntry>& entries,
                                  std::size_t from, std::size_t end) const;
    bool hasExternalReplacement(const std::vector<ProjectedSessionEntry>& entries, std::size_t from,
                                std::size_t end) const;

    AgentMessageCodec m_codec;
    AgentTokenEstimator m_estimator;
    SessionProjector m_projector;
};

bool CutPointFinder::isCutPointRole(const std::string& role) const {
    return role == "user" || role == "assistant" || role == "bashExecution" || role == "custom" ||
           role == "branchSummary" || role == "compactionSummary";
}

bool CutPointFinder::isTurnStartRole(const std::string& role) const {
    return role == "user" || role == "bashExecution" || role == "custom" ||
           role == "branchSummary" || role == "compactionSummary";
}

bool CutPointFinder::hasCutPointMessage(const ProjectedSessionEntry& entry) const {
    return std::ranges::any_of(entry.messages, [&](const AgentMessage& message) {
        return isCutPointRole(m_codec.roleOf(message));
    });
}

bool CutPointFinder::startsTurn(const ProjectedSessionEntry& entry) const {
    if (entry.sourceEntry.type == "compaction") {
        return false;
    }
    return std::ranges::any_of(entry.messages, [&](const AgentMessage& message) {
        return isTurnStartRole(m_codec.roleOf(message));
    });
}

int CutPointFinder::turnStartBefore(const std::vector<ProjectedSessionEntry>& entries,
                                    std::size_t index, std::size_t start) const {
    for (std::size_t i = index + 1; i-- > start;) {
        if (startsTurn(entries[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<std::size_t> CutPointFinder::cutPoints(const std::vector<ProjectedSessionEntry>& entries,
                                                   std::size_t start, std::size_t end) const {
    std::vector<std::size_t> points;
    for (std::size_t i = start; i < end; ++i) {
        if (entries[i].sourceEntry.type != "compaction" && hasCutPointMessage(entries[i])) {
            points.push_back(i);
        }
    }
    return points;
}

std::optional<std::size_t> CutPointFinder::budgetCut(
    const std::vector<ProjectedSessionEntry>& entries, const std::vector<std::size_t>& points,
    std::size_t start, std::size_t end, std::int64_t keepRecentTokens) const {
    std::int64_t accumulated = 0;
    for (std::size_t i = end; i-- > start;) {
        std::int64_t tokens = 0;
        for (const auto& message : entries[i].messages) {
            tokens += m_estimator.messageTokens(message);
        }
        if (tokens == 0) {
            continue;
        }
        accumulated += tokens;
        if (accumulated >= keepRecentTokens) {
            const auto found = std::ranges::find_if(points, [&](std::size_t p) { return p >= i; });
            return found == points.end() ? points.back() : *found;
        }
    }
    return std::nullopt;
}

bool CutPointFinder::isVisible(const ProjectedSessionEntry& entry) const {
    return entry.sourceEntry.type != "context_edit" &&
           !m_projector.entryToMessages(entry.sourceEntry).empty();
}

bool CutPointFinder::isOmitted(const ProjectedSessionEntry& entry) const {
    return isVisible(entry) && entry.messages.empty();
}

bool CutPointFinder::isAssistantMessageEntry(const ProjectedSessionEntry& entry) const {
    const Json& body = entry.sourceEntry.body;
    return entry.sourceEntry.type == "message" && body.contains("message") &&
           body["message"].is_object() && body["message"].value("role", "") == "assistant";
}

bool CutPointFinder::hasExternalReplacement(const std::vector<ProjectedSessionEntry>& entries,
                                            std::size_t from, std::size_t end) const {
    std::set<std::string> omittedIds;
    for (std::size_t i = from; i < end; ++i) {
        if (isOmitted(entries[i])) {
            omittedIds.insert(entries[i].sourceEntry.id);
        }
    }
    for (std::size_t i = from; i < end; ++i) {
        const Json& body = entries[i].sourceEntry.body;
        const bool replaces = body.contains("replacement") && !body["replacement"].is_null();
        if (entries[i].sourceEntry.type == "context_edit" && replaces &&
            !omittedIds.contains(body.value("targetId", ""))) {
            return true;
        }
    }
    return false;
}

bool CutPointFinder::isRecoveryOmissionSuffix(const std::vector<ProjectedSessionEntry>& entries,
                                              std::size_t from, std::size_t end) const {
    if (hasExternalReplacement(entries, from, end)) {
        return false;
    }
    bool omittedAssistant = false;
    for (std::size_t i = from; i < end; ++i) {
        const auto& entry = entries[i];
        if (entry.sourceEntry.type == "compaction" || (isVisible(entry) && !isOmitted(entry))) {
            return false;
        }
        omittedAssistant = omittedAssistant || (isAssistantMessageEntry(entry) && isOmitted(entry));
    }
    return omittedAssistant;
}

CutPointResult CutPointFinder::find(const std::vector<ProjectedSessionEntry>& entries,
                                    std::size_t start, std::size_t end,
                                    std::int64_t keepRecentTokens) const {
    const std::vector<std::size_t> points = cutPoints(entries, start, end);
    if (points.empty()) {
        return CutPointResult{start, -1, false};
    }
    const auto budget = budgetCut(entries, points, start, end, keepRecentTokens);
    std::size_t cut = budget.value_or(points.front());
    // A recovery attempt and its omission edits are invisible after the last visible input:
    // move past them only when the whole suffix is such a closed group.
    if (budget && isRecoveryOmissionSuffix(entries, cut + 1, end)) {
        ++cut;
    }
    while (cut > start) {
        const auto& previous = entries[cut - 1];
        if (previous.sourceEntry.type == "compaction" || !previous.messages.empty()) {
            break;
        }
        --cut;
    }
    const bool atTurnStart = startsTurn(entries[cut]);
    const int turnStart = atTurnStart ? -1 : turnStartBefore(entries, cut, start);
    return CutPointResult{cut, turnStart, !atTurnStart && turnStart != -1};
}
