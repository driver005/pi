export module pi.support.transcript_service;

import std;
export import pi.chord.i_remote_service;
export import pi.session.i_agent_session;
export import pi.support.agent_message_codec;
export import pi.support.replicated_state;

/**
 * The `pi.transcript` service: the session's conversation as replicated state, `{messages,
 * isStreaming}`, with messages in the session-file JSON form. It is refreshed on every agent event,
 * so a subscriber sees streaming text grow as small append operations. This is the AgentSession
 * view of the conversation; the TS service serves the durable ConversationView instead.
 */
export class TranscriptService : public IRemoteService {
public:
    explicit TranscriptService(IAgentSession& session);
    ~TranscriptService() override;

    TranscriptService(const TranscriptService&) = delete;
    TranscriptService& operator=(const TranscriptService&) = delete;

    std::map<std::string, Method> methods() override;
    std::map<std::string, IReplicatedState*> states() override;

private:
    Json view() const;
    void refresh();

    IAgentSession& m_session;
    AgentMessageCodec m_codec;
    ReplicatedState m_state;
    IAgentSession::ListenerId m_listener = 0;
};

TranscriptService::TranscriptService(IAgentSession& session) : m_session(session), m_state(view()) {
    m_listener = m_session.subscribe([this](const AgentSessionEvent& event) {
        if (event.type == SessionEventType::Agent || event.type == SessionEventType::AgentEnd ||
            event.type == SessionEventType::AgentSettled || event.type == SessionEventType::EntryAppended ||
            event.type == SessionEventType::CompactionEnd) {
            refresh();
        }
    });
}

TranscriptService::~TranscriptService() {
    m_session.unsubscribe(m_listener);
}

Json TranscriptService::view() const {
    return Json{{"messages", m_codec.listToJson(m_session.messages())}, {"isStreaming", m_session.isStreaming()}};
}

void TranscriptService::refresh() {
    m_state.change(ServiceContext{std::make_shared<AbortSignal>()}, [this](Json& draft) { draft = view(); });
}

std::map<std::string, IRemoteService::Method> TranscriptService::methods() {
    return {};
}

std::map<std::string, IReplicatedState*> TranscriptService::states() {
    return {{"state", &m_state}};
}
