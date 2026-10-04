module;

#include <cstdint>

export module pi.support.agent_token_estimator;

import std;
import pi.support.json_writer;
export import pi.support.agent_message_codec;
export import pi.support.transcript_normalizer;
export import pi.types.agent_message;
export import pi.types.compaction_settings;
export import pi.types.context_usage_estimate;
export import pi.types.session_entry;
export import pi.types.session_projection;
export import pi.types.usage;

/**
 * Context-size estimates over the application transcript: the last reported usage plus a
 * chars/4 estimate of what came after it. Port of the token functions in compaction/compaction.ts.
 */
export class AgentTokenEstimator {
public:
    std::int64_t contextTokens(const Usage& usage) const {
        return usage.totalTokens != 0
                   ? usage.totalTokens
                   : usage.input + usage.output + usage.cacheRead + usage.cacheWrite;
    }

    std::int64_t messageTokens(const AgentMessage& message) const {
        if (const auto* system = std::get_if<SystemMessage>(&message)) {
            return charsToTokens(systemChars(*system));
        }
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            if (const auto* text = std::get_if<std::string>(&user->content)) {
                return charsToTokens(length(*text));
            }
            return charsToTokens(blocksChars(std::get<std::vector<UserContentBlock>>(user->content)));
        }
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            return assistantTokens(*assistant);
        }
        if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
            return charsToTokens(blocksChars(result->content));
        }
        return customTokens(std::get<CustomMessage>(message));
    }

    /** Usage of an assistant message when it is valid (not aborted/error, non-zero). */
    std::optional<Usage> assistantUsage(const AgentMessage& message) const {
        const auto* assistant = std::get_if<AssistantMessage>(&message);
        if (assistant == nullptr || assistant->stopReason == StopReason::Aborted ||
            assistant->stopReason == StopReason::Error || contextTokens(assistant->usage) <= 0) {
            return std::nullopt;
        }
        return assistant->usage;
    }

    std::optional<Usage> lastAssistantUsage(const std::vector<SessionEntry>& entries) const {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->type != "message" || !it->body.contains("message")) {
                continue;
            }
            const auto decoded = m_codec.fromJson(it->body["message"]);
            if (decoded) {
                if (auto usage = assistantUsage(*decoded)) {
                    return usage;
                }
            }
        }
        return std::nullopt;
    }

    ContextUsageEstimate estimate(const std::vector<AgentMessage>& messages) const {
        ContextUsageEstimate out;
        const auto info = lastUsageInfo(messages);
        if (!info) {
            for (const auto& message : messages) {
                out.trailingTokens += messageTokens(message);
            }
            out.tokens = out.trailingTokens;
            return out;
        }
        out.usageTokens = contextTokens(info->first);
        for (std::size_t i = static_cast<std::size_t>(info->second) + 1; i < messages.size(); ++i) {
            out.trailingTokens += messageTokens(messages[i]);
        }
        out.tokens = out.usageTokens + out.trailingTokens;
        out.lastUsageIndex = info->second;
        return out;
    }

    /** Like estimate, but ignores usage recorded before a later context edit or compaction. */
    ContextUsageEstimate estimateProjected(const SessionProjection& projection, const std::vector<SessionEntry>& branchEntries) const {
        const ContextUsageEstimate direct = estimate(projection.messages);
        if (direct.lastUsageIndex >= 0 &&
            usageIsFresh(usageEntryId(projection, direct.lastUsageIndex), branchEntries)) {
            return direct;
        }
        ContextUsageEstimate out;
        out.tokens = systemReplayTokens(projection.messages);
        out.trailingTokens = out.tokens;
        return out;
    }

    bool shouldCompact(std::int64_t contextTokens, std::int64_t contextWindow, const CompactionSettings& settings) const {
        return settings.enabled && contextTokens > contextWindow - settings.reserveTokens;
    }

private:
    static constexpr std::size_t ImageChars = 4800;

    std::int64_t charsToTokens(std::size_t chars) const {
        return static_cast<std::int64_t>((chars + 3) / 4);
    }

    std::size_t length(const std::string& text) const {
        std::size_t units = 0;
        for (const unsigned char byte : text) {
            if ((byte & 0xC0) == 0x80) {
                continue;
            }
            units += byte >= 0xF0 ? 2 : 1;
        }
        return units;
    }

    std::size_t blocksChars(const std::vector<UserContentBlock>& blocks) const {
        std::size_t chars = 0;
        for (const auto& block : blocks) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                chars += length(text->text);
            } else {
                chars += ImageChars;
            }
        }
        return chars;
    }

    std::size_t systemChars(const SystemMessage& message) const {
        std::size_t chars = 0;
        if (const auto* text = std::get_if<std::string>(&message.content)) {
            chars += length(*text);
        } else {
            for (const auto& block : std::get<std::vector<TextContent>>(message.content)) {
                chars += length(block.text);
            }
        }
        if (message.sections) {
            for (const auto& [name, value] : *message.sections) {
                chars += value ? length(*value) : 0;
            }
        }
        if (message.toolsAdded) {
            Json tools = Json::array();
            for (const auto& tool : *message.toolsAdded) {
                tools.push_back(m_messages.toJson(tool));
            }
            chars += jsonLength(tools);
        }
        return chars;
    }

    std::int64_t assistantTokens(const AssistantMessage& message) const {
        std::size_t chars = 0;
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<TextContent>(&block)) {
                chars += length(text->text);
            } else if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                chars += length(thinking->thinking);
            } else if (const auto* call = std::get_if<ToolCall>(&block)) {
                chars += length(call->name) + jsonLength(call->arguments);
            }
        }
        return charsToTokens(chars);
    }

    std::int64_t customTokens(const CustomMessage& message) const {
        const Json& data = message.data;
        const auto text = [&](const char* key) -> std::size_t {
            const auto found = data.find(key);
            return found != data.end() && found->is_string() ? length(found->get<std::string>()) : 0;
        };
        if (message.role == "custom") {
            const auto content = data.find("content");
            if (content == data.end()) {
                return 0;
            }
            if (content->is_string()) {
                return charsToTokens(length(content->get<std::string>()));
            }
            std::size_t chars = 0;
            for (const auto& block : *content) {
                const std::string type = block.value("type", "");
                if (type == "text" && block.contains("text") && block["text"].is_string()) {
                    chars += length(block["text"].get<std::string>());
                } else if (type == "image") {
                    chars += ImageChars;
                }
            }
            return charsToTokens(chars);
        }
        if (message.role == "bashExecution") {
            return charsToTokens(text("command") + text("output"));
        }
        if (message.role == "branchSummary" || message.role == "compactionSummary") {
            return charsToTokens(text("summary"));
        }
        return 0;
    }

    std::size_t jsonLength(const Json& value) const {
        return length(m_writer.compact(value));
    }

    std::optional<std::pair<Usage, int>> lastUsageInfo(const std::vector<AgentMessage>& messages) const {
        for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
            if (auto usage = assistantUsage(messages[static_cast<std::size_t>(i)])) {
                return std::make_pair(*usage, i);
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> usageEntryId(const SessionProjection& projection, int messageIndex) const {
        std::size_t start = 0;
        for (const auto& entry : projection.entries) {
            const std::size_t next = start + entry.messages.size();
            if (static_cast<std::size_t>(messageIndex) < next) {
                return entry.sourceEntry.id;
            }
            start = next;
        }
        return std::nullopt;
    }

    bool usageIsFresh(const std::optional<std::string>& entryId, const std::vector<SessionEntry>& branchEntries) const {
        int usageIndex = -1;
        int invalidating = -1;
        for (std::size_t i = 0; i < branchEntries.size(); ++i) {
            if (entryId && branchEntries[i].id == *entryId && usageIndex < 0) {
                usageIndex = static_cast<int>(i);
            }
            if (branchEntries[i].type == "context_edit" || branchEntries[i].type == "compaction") {
                invalidating = static_cast<int>(i);
            }
        }
        return usageIndex > invalidating;
    }

    std::int64_t systemReplayTokens(const std::vector<AgentMessage>& messages) const {
        std::vector<Message> systems;
        std::int64_t tokens = 0;
        for (const auto& message : messages) {
            if (const auto* system = std::get_if<SystemMessage>(&message)) {
                systems.emplace_back(*system);
            } else {
                tokens += messageTokens(message);
            }
        }
        if (const auto current = m_normalizer.currentSystemMessage(systems)) {
            tokens += charsToTokens(systemChars(*current));
        }
        return tokens;
    }

    AgentMessageCodec m_codec;
    MessageCodec m_messages;
    JsonWriter m_writer;
    TranscriptNormalizer m_normalizer;
};

/** UTF-16 code units, the length JavaScript reports, so estimates match the TypeScript side. */
