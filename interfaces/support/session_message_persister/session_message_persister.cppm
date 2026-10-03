export module pi.support.session_message_persister;

import std;
export import pi.session.i_session_manager;
export import pi.types.agent_message;

/**
 * Stores a finished agent message in the session tree: LLM messages as message entries, custom
 * messages from plugins as custom_message entries. Other application messages (bash results,
 * summaries) are stored by the code that creates them.
 */
export class SessionMessagePersister {
public:
    explicit SessionMessagePersister(ISessionManager& session)
        : m_session(session) {}

    /** The new entry's id, or nullopt when the message kind is stored elsewhere. */
    std::optional<std::string> persist(const AgentMessage& message) {
        if (const auto* custom = std::get_if<CustomMessage>(&message)) {
            if (custom->role != "custom") {
                return std::nullopt;
            }
            const Json& data = custom->data;
            const auto id = m_session.appendCustomMessageEntry(
                data.value("customType", ""), data.contains("content") ? data["content"] : Json::array(),
                data.value("display", true), data.contains("details") ? data["details"] : Json());
            return id ? std::optional<std::string>(*id) : std::nullopt;
        }
        const auto id = m_session.appendMessage(message);
        return id ? std::optional<std::string>(*id) : std::nullopt;
    }

private:
    ISessionManager& m_session;
};
