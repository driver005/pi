export module pi.testing.session_runtime_factory;

import std;
export import pi.agent.i_agent_factory;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.platform.i_sleeper;
export import pi.session.i_session_runtime_factory;
import pi.ai.faux_provider;
import pi.testing.test_session_handle;

/** ISessionRuntimeFactory building TestSessionHandle sessions; counts the requests it served. */
export class SessionRuntimeFactory : public ISessionRuntimeFactory {
public:
    SessionRuntimeFactory(IAgentFactory& agents, FauxProvider& provider, IFileSystem& files, const IClock& clock, IIdGenerator& ids, ISleeper& sleeper)
        : m_agents(agents),
          m_provider(provider),
          m_files(files),
          m_clock(clock),
          m_ids(ids),
          m_sleeper(sleeper) {}

    Result<std::unique_ptr<ISessionRuntimeHandle>> create(SessionRuntimeRequest request) override {
        if (m_failure) {
            const std::string message = *std::exchange(m_failure, std::nullopt);
            return std::unexpected(Error{"create_failed", message});
        }
        m_reasons.push_back(request.startReason);
        return std::unique_ptr<ISessionRuntimeHandle>(std::make_unique<TestSessionHandle>(
            std::move(request), m_agents, m_provider, m_files, m_clock, m_ids, m_sleeper, &m_denyReplacement));
    }

    /** Start reasons of every created session, in order. */
    std::vector<std::string> reasons() const {
        return m_reasons;
    }

    /** Makes the sessions it created refuse being switched or forked (a plugin cancelling `session_before_switch` and `session_before_fork`). */
    void denyReplacement(bool deny) {
        m_denyReplacement = deny;
    }

    /** Makes the next create() fail with this message. */
    void failNext(const std::string& message) {
        m_failure = message;
    }

private:
    IAgentFactory& m_agents;
    FauxProvider& m_provider;
    IFileSystem& m_files;
    const IClock& m_clock;
    IIdGenerator& m_ids;
    ISleeper& m_sleeper;
    std::vector<std::string> m_reasons;
    std::optional<std::string> m_failure;
    bool m_denyReplacement = false;
};
