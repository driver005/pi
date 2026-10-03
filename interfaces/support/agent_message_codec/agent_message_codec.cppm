module;

#include <cstdint>

export module pi.support.agent_message_codec;

import std;
export import pi.support.message_codec;
export import pi.types.agent_message;
export import pi.types.json;
export import pi.types.result;

/**
 * AgentMessage <-> JSON as stored in session files. LLM roles go through MessageCodec; any other
 * role (bashExecution, custom, branchSummary, compactionSummary, ...) becomes a CustomMessage that
 * keeps every field, so unknown message kinds survive a read-modify-write.
 */
export class AgentMessageCodec {
public:
    Json toJson(const AgentMessage& message) const;
    Result<AgentMessage> fromJson(const Json& json) const;
    Result<std::vector<AgentMessage>> listFromJson(const Json& json) const;
    Json listToJson(const std::vector<AgentMessage>& messages) const;

    /** The role name of any message ("custom" kinds report their own role). */
    std::string roleOf(const AgentMessage& message) const;

    /** Timestamp of any message, epoch milliseconds. */
    std::int64_t timestampOf(const AgentMessage& message) const;

private:
    bool isLlmRole(const std::string& role) const;

    MessageCodec m_codec;
};

bool AgentMessageCodec::isLlmRole(const std::string& role) const {
    return role == "system" || role == "user" || role == "assistant" || role == "toolResult";
}

Json AgentMessageCodec::toJson(const AgentMessage& message) const {
    if (const auto* custom = std::get_if<CustomMessage>(&message)) {
        Json out = Json::object();
        out["role"] = custom->role;
        if (custom->data.is_object()) {
            for (const auto& entry : custom->data.items()) {
                const std::string& key = entry.key();
                const Json& value = entry.value();
                out[key] = value;
            }
        }
        out["timestamp"] = custom->timestamp;
        return out;
    }
    return std::visit(
        [this](const auto& value) -> Json {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, CustomMessage>) {
                return Json::object();
            } else {
                return m_codec.toJson(value);
            }
        },
        message);
}

Result<AgentMessage> AgentMessageCodec::fromJson(const Json& json) const {
    if (!json.is_object() || !json.contains("role") || !json["role"].is_string()) {
        return std::unexpected(Error{"invalid_message", "message must be an object with a role"});
    }
    const std::string role = json["role"].get<std::string>();
    if (isLlmRole(role)) {
        auto message = m_codec.messageFromJson(json);
        if (!message) {
            return std::unexpected(message.error());
        }
        return std::visit([](auto&& value) -> AgentMessage { return std::move(value); },
                          std::move(*message));
    }
    CustomMessage custom;
    custom.role = role;
    for (const auto& entry : json.items()) {
        const std::string& key = entry.key();
        const Json& value = entry.value();
        if (key == "role") {
            continue;
        }
        if (key == "timestamp") {
            custom.timestamp = value.is_number() ? value.get<std::int64_t>() : 0;
            continue;
        }
        custom.data[key] = value;
    }
    return AgentMessage(std::move(custom));
}

Result<std::vector<AgentMessage>> AgentMessageCodec::listFromJson(const Json& json) const {
    if (!json.is_array()) {
        return std::unexpected(Error{"invalid_message", "messages must be an array"});
    }
    std::vector<AgentMessage> messages;
    for (const auto& item : json) {
        auto message = fromJson(item);
        if (!message) {
            return std::unexpected(message.error());
        }
        messages.push_back(std::move(*message));
    }
    return messages;
}

Json AgentMessageCodec::listToJson(const std::vector<AgentMessage>& messages) const {
    Json out = Json::array();
    for (const auto& message : messages) {
        out.push_back(toJson(message));
    }
    return out;
}

std::string AgentMessageCodec::roleOf(const AgentMessage& message) const {
    if (const auto* custom = std::get_if<CustomMessage>(&message)) {
        return custom->role;
    }
    if (std::holds_alternative<SystemMessage>(message)) {
        return "system";
    }
    if (std::holds_alternative<UserMessage>(message)) {
        return "user";
    }
    if (std::holds_alternative<AssistantMessage>(message)) {
        return "assistant";
    }
    return "toolResult";
}

std::int64_t AgentMessageCodec::timestampOf(const AgentMessage& message) const {
    return std::visit([](const auto& value) -> std::int64_t { return value.timestamp; }, message);
}
