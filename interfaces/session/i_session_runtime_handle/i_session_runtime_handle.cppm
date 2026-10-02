export module pi.session.i_session_runtime_handle;

import std;
export import pi.session.i_agent_session;
export import pi.session.i_session_manager;
export import pi.types.runtime_diagnostic;

/** One live session together with the services it owns (settings, resources, tools, model runtime). */
export class ISessionRuntimeHandle {
public:
    virtual ~ISessionRuntimeHandle() = default;

    virtual IAgentSession& session() = 0;
    virtual ISessionManager& sessionManager() = 0;
    virtual std::string cwd() const = 0;
    virtual std::string agentDir() const = 0;
    virtual std::vector<RuntimeDiagnostic> diagnostics() const = 0;
    /** After the session was disposed: hands the session tree to the next runtime (in-memory forks). */
    virtual std::unique_ptr<ISessionManager> releaseSessionManager() = 0;
};
