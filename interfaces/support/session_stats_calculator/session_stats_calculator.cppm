export module pi.support.session_stats_calculator;

import std;
export import pi.support.agent_message_codec;
export import pi.support.message_codec;
export import pi.types.session_entry;
export import pi.types.session_stats;

/**
 * Aggregates a session: message counts and token/cost totals over every entry, including history
 * that compaction removed from context, so totals reflect what was billed.
 */
export class SessionStatsCalculator {
public:
    /** Counts and totals only; the caller sets session file, id and context usage. */
    SessionStats calculate(const std::vector<SessionEntry>& entries) const {
        SessionStats stats;
        for (const auto& entry : entries) {
            const bool carriesUsage = entry.type == "usage" || entry.type == "branch_summary" ||
                                      entry.type == "compaction";
            if (carriesUsage && entry.body.contains("usage") && entry.body["usage"].is_object()) {
                addUsage(entry.body["usage"], stats);
            }
            if (entry.type == "message" && entry.body.contains("message") && entry.body["message"].is_object()) {
                addMessage(entry.body["message"], stats);
            }
        }
        stats.totalTokens = stats.inputTokens + stats.outputTokens + stats.cacheReadTokens + stats.cacheWriteTokens;
        return stats;
    }

private:
    void addUsage(const Json& usage, SessionStats& stats) const {
        const auto decoded = m_codec.usageFromJson(usage);
        if (!decoded) {
            return;
        }
        stats.inputTokens += decoded->input;
        stats.outputTokens += decoded->output;
        stats.cacheReadTokens += decoded->cacheRead;
        stats.cacheWriteTokens += decoded->cacheWrite;
        stats.cost += decoded->cost.total;
    }

    void addMessage(const Json& message, SessionStats& stats) const {
        ++stats.totalMessages;
        const std::string role = message.value("role", "");
        if (role == "user") {
            ++stats.userMessages;
        } else if (role == "toolResult") {
            ++stats.toolResults;
            if (message.contains("usage") && message["usage"].is_object()) {
                addUsage(message["usage"], stats);
            }
        } else if (role == "assistant") {
            ++stats.assistantMessages;
            if (message.contains("content") && message["content"].is_array()) {
                for (const auto& block : message["content"]) {
                    stats.toolCalls += block.value("type", "") == "toolCall" ? 1 : 0;
                }
            }
            if (message.contains("usage") && message["usage"].is_object()) {
                addUsage(message["usage"], stats);
            }
        }
    }

    MessageCodec m_codec;
};
