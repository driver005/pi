module;

#include <cstdint>

export module pi.support.agent_message_converter;

import std;
export import pi.support.iso_timestamp;
export import pi.support.message_codec;
export import pi.types.agent_message;
export import pi.types.json;
export import pi.types.message;

/**
 * Converts the application-level transcript (bashExecution, custom, branchSummary,
 * compactionSummary roles) into provider messages, and builds those custom messages from session
 * entries. Port of core/messages.ts.
 */
export class AgentMessageConverter {
public:
    /** Provider-visible messages; messages excluded from context are dropped. */
    std::vector<Message> convert(const std::vector<AgentMessage>& messages) const;

    /** User-message text for a bashExecution message (its `data` object). */
    std::string bashExecutionText(const Json& data) const;

    CustomMessage branchSummary(const std::string& summary, const std::optional<std::string>& fromId,
                                const std::string& isoTimestamp) const;
    CustomMessage compactionSummary(const std::string& summary, std::int64_t tokensBefore,
                                    const std::string& isoTimestamp) const;
    /** `content` is the entry's JSON content: a string or an array of content blocks. */
    CustomMessage custom(const std::string& customType, const Json& content, bool display,
                         const Json& details, const std::string& isoTimestamp) const;

private:
    std::optional<Message> convertOne(const AgentMessage& message) const;
    std::optional<Message> convertCustom(const CustomMessage& message) const;
    Message userText(const std::string& text, std::int64_t timestamp) const;
    std::string stringField(const Json& data, const char* key) const;
    std::int64_t timestampMs(const std::string& iso) const;

    MessageCodec m_codec;
    IsoTimestamp m_iso;
};

std::int64_t AgentMessageConverter::timestampMs(const std::string& iso) const {
    return m_iso.parse(iso).value_or(0);
}

std::string AgentMessageConverter::stringField(const Json& data, const char* key) const {
    const auto found = data.find(key);
    return found != data.end() && found->is_string() ? found->get<std::string>() : std::string();
}

Message AgentMessageConverter::userText(const std::string& text, std::int64_t timestamp) const {
    UserMessage message;
    message.content = std::vector<UserContentBlock>{TextContent{text, std::nullopt}};
    message.timestamp = timestamp;
    return message;
}

std::string AgentMessageConverter::bashExecutionText(const Json& data) const {
    const std::string command = stringField(data, "command");
    const std::string output = stringField(data, "output");
    std::string text = "Ran `" + command + "`\n";
    text += output.empty() ? "(no output)" : "```\n" + output + "\n```";
    const bool cancelled = data.value("cancelled", false);
    const auto exitCode = data.find("exitCode");
    if (cancelled) {
        text += "\n\n(command cancelled)";
    } else if (exitCode != data.end() && exitCode->is_number() && exitCode->get<std::int64_t>() != 0) {
        text += "\n\nCommand exited with code " + std::to_string(exitCode->get<std::int64_t>());
    }
    const std::string fullPath = stringField(data, "fullOutputPath");
    if (data.value("truncated", false) && !fullPath.empty()) {
        text += "\n\n[Output truncated. Full output: " + fullPath + "]";
    }
    return text;
}

CustomMessage AgentMessageConverter::branchSummary(const std::string& summary,
                                                   const std::optional<std::string>& fromId,
                                                   const std::string& isoTimestamp) const {
    CustomMessage message;
    message.role = "branchSummary";
    message.data = Json::object();
    message.data["summary"] = summary;
    message.data["fromId"] = fromId ? Json(*fromId) : Json(nullptr);
    message.timestamp = timestampMs(isoTimestamp);
    return message;
}

CustomMessage AgentMessageConverter::compactionSummary(const std::string& summary,
                                                       std::int64_t tokensBefore,
                                                       const std::string& isoTimestamp) const {
    CustomMessage message;
    message.role = "compactionSummary";
    message.data = Json::object();
    message.data["summary"] = summary;
    message.data["tokensBefore"] = tokensBefore;
    message.timestamp = timestampMs(isoTimestamp);
    return message;
}

CustomMessage AgentMessageConverter::custom(const std::string& customType, const Json& content,
                                            bool display, const Json& details,
                                            const std::string& isoTimestamp) const {
    CustomMessage message;
    message.role = "custom";
    message.data = Json::object();
    message.data["customType"] = customType;
    message.data["content"] = content;
    message.data["display"] = display;
    if (!details.is_null()) {
        message.data["details"] = details;
    }
    message.timestamp = timestampMs(isoTimestamp);
    return message;
}

std::optional<Message> AgentMessageConverter::convertCustom(const CustomMessage& message) const {
    const Json& data = message.data;
    if (message.role == "bashExecution") {
        if (data.value("excludeFromContext", false)) {
            return std::nullopt;
        }
        return userText(bashExecutionText(data), message.timestamp);
    }
    if (message.role == "custom") {
        UserMessage user;
        user.timestamp = message.timestamp;
        const auto content = data.find("content");
        if (content != data.end() && content->is_string()) {
            user.content = content->get<std::string>();
        } else if (content != data.end() && content->is_array()) {
            std::vector<UserContentBlock> blocks;
            for (const auto& item : *content) {
                if (auto block = m_codec.userBlockFromJson(item)) {
                    blocks.push_back(*block);
                }
            }
            user.content = std::move(blocks);
        }
        return Message(user);
    }
    if (message.role == "branchSummary") {
        return userText("The following is a summary of a branch that this conversation came back from:\n\n<summary>\n" +
                            stringField(data, "summary") + "</summary>",
                        message.timestamp);
    }
    if (message.role == "compactionSummary") {
        return userText("The conversation history before this point was compacted into the following summary:\n\n<summary>\n" +
                            stringField(data, "summary") + "\n</summary>",
                        message.timestamp);
    }
    return std::nullopt;
}

std::optional<Message> AgentMessageConverter::convertOne(const AgentMessage& message) const {
    if (const auto* custom = std::get_if<CustomMessage>(&message)) {
        return convertCustom(*custom);
    }
    return std::visit(
        [](const auto& value) -> std::optional<Message> {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, CustomMessage>) {
                return std::nullopt;
            } else {
                return Message(value);
            }
        },
        message);
}

std::vector<Message> AgentMessageConverter::convert(const std::vector<AgentMessage>& messages) const {
    std::vector<Message> out;
    for (const auto& message : messages) {
        if (auto converted = convertOne(message)) {
            out.push_back(std::move(*converted));
        }
    }
    return out;
}
