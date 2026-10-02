export module pi.support.session_context_refresher;

import std;
export import pi.agent.i_agent;
export import pi.session.i_session_manager;

/**
 * Keeps the agent's transcript equal to the session tree's projection: after anything changes the
 * tree (compaction, context edits, bash results, navigation) the agent reloads its messages.
 */
export class SessionContextRefresher {
public:
    SessionContextRefresher(IAgent& agent, ISessionManager& session);

    void refresh();

private:
    IAgent& m_agent;
    ISessionManager& m_session;
};

SessionContextRefresher::SessionContextRefresher(IAgent& agent, ISessionManager& session)
    : m_agent(agent), m_session(session) {}

void SessionContextRefresher::refresh() {
    m_agent.setMessages(m_session.buildSessionProjection().messages);
}
