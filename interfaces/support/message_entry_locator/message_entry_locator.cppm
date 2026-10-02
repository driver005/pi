module;

#include <nlohmann/json.hpp>

export module pi.support.message_entry_locator;

import std;
export import pi.support.agent_message_codec;
export import pi.types.session_entry;

/**
 * Finds the session entry that stores a given message. C++ messages have no identity, so the
 * entry is found by comparing the stored message JSON, newest entry first.
 */
export class MessageEntryLocator {
public:
    std::optional<std::string> find(const std::vector<SessionEntry>& branch, const AgentMessage& message) const;

private:
    AgentMessageCodec m_codec;
};

std::optional<std::string> MessageEntryLocator::find(const std::vector<SessionEntry>& branch,
                                                     const AgentMessage& message) const {
    const Json wanted = m_codec.toJson(message);
    for (auto it = branch.rbegin(); it != branch.rend(); ++it) {
        if (it->type == "message" && it->body.contains("message") && it->body["message"] == wanted) {
            return it->id;
        }
    }
    return std::nullopt;
}
