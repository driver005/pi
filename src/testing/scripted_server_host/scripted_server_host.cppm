export module pi.testing.scripted_server_host;

import std;
export import pi.server.i_server_host;
export import pi.testing.scripted_session_handle;

/**
 * An IServerHost (and its server-wide service host) for tests: sessions are added by id, resolved
 * by exact id or unique prefix, and every call goes to handlers the test sets.
 */
export class ScriptedServerHost : public IServerHost, public IServerServiceHost {
public:
    std::shared_ptr<ScriptedSessionState> addSession(const std::string& id);
    void setServerHandler(ScriptedServiceAttachment::Handler handler);
    /** The presentation of the most recently attached client, or null. */
    IServerPresentation* presentation();
    int serverAttachments() const;
    int serverReleases() const;
    void failServerAttach(const Error& error);

    IServerServiceHost& serverServices() override;
    Result<std::string> resolveSession(const std::string& sessionId, const ServiceContext& context) override;
    Result<std::shared_ptr<IRoutedSessionHandle>> openSession(const std::string& sessionId,
                                                              const ServiceContext& context) override;
    Result<std::unique_ptr<IServiceAttachment>> attachClient(IServerPresentation& presentation,
                                                             const ServiceContext& context) override;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<ScriptedSessionState>> m_sessions;
    ScriptedServiceAttachment::Handler m_serverHandler;
    IServerPresentation* m_presentation = nullptr;
    std::optional<Error> m_attachFailure;
    std::atomic<int> m_serverAttachments{0};
    std::atomic<int> m_serverReleases{0};
};

std::shared_ptr<ScriptedSessionState> ScriptedServerHost::addSession(const std::string& id) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto state = std::make_shared<ScriptedSessionState>();
    state->id = id;
    m_sessions[id] = state;
    return state;
}

void ScriptedServerHost::setServerHandler(ScriptedServiceAttachment::Handler handler) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_serverHandler = std::move(handler);
}

IServerPresentation* ScriptedServerHost::presentation() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_presentation;
}

int ScriptedServerHost::serverAttachments() const {
    return m_serverAttachments.load();
}

int ScriptedServerHost::serverReleases() const {
    return m_serverReleases.load();
}

void ScriptedServerHost::failServerAttach(const Error& error) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_attachFailure = error;
}

IServerServiceHost& ScriptedServerHost::serverServices() {
    return *this;
}

Result<std::string> ScriptedServerHost::resolveSession(const std::string& sessionId, const ServiceContext&) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sessions.contains(sessionId)) {
        return sessionId;
    }
    std::vector<std::string> matches;
    for (const auto& entry : m_sessions) {
        if (!sessionId.empty() && entry.first.starts_with(sessionId)) {
            matches.push_back(entry.first);
        }
    }
    if (matches.size() == 1) {
        return matches.front();
    }
    if (matches.size() > 1) {
        return std::unexpected(Error{"session_ambiguous", "Session ID matches more than one session"});
    }
    return std::unexpected(Error{"session_not_found", "Session was not found"});
}

Result<std::shared_ptr<IRoutedSessionHandle>> ScriptedServerHost::openSession(const std::string& sessionId,
                                                                              const ServiceContext&) {
    std::shared_ptr<ScriptedSessionState> state;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_sessions.find(sessionId);
        if (found == m_sessions.end()) {
            return std::unexpected(Error{"session_not_found", "Session was not found"});
        }
        state = found->second;
    }
    ++state->opened;
    if (state->openFailure) {
        return std::unexpected(*state->openFailure);
    }
    return std::shared_ptr<IRoutedSessionHandle>(std::make_shared<ScriptedSessionHandle>(state));
}

Result<std::unique_ptr<IServiceAttachment>> ScriptedServerHost::attachClient(IServerPresentation& presentation,
                                                                             const ServiceContext&) {
    ScriptedServiceAttachment::Handler handler;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_attachFailure) {
            return std::unexpected(*m_attachFailure);
        }
        m_presentation = &presentation;
        handler = m_serverHandler;
    }
    ++m_serverAttachments;
    return std::unique_ptr<IServiceAttachment>(
        std::make_unique<ScriptedServiceAttachment>(std::move(handler), [this] { ++m_serverReleases; }));
}
