export module pi.session.i_session_runtime_handle;

import std;
export import pi.session.i_agent_session;
export import pi.session.i_session_manager;
export import pi.types.fork_result;
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
    /** Asks the plugins whether the session may be replaced (`session_before_switch`; reason "new" or "resume"); false: a plugin cancelled. */
    virtual bool allowSwitch(const std::string& reason, const std::optional<std::string>& targetSessionFile) {
        (void)reason;
        (void)targetSessionFile;
        return true;
    }
    /** Asks the plugins whether the session may be forked (`session_before_fork`); false: a plugin cancelled. */
    virtual bool allowFork(const std::string& entryId, ForkPosition position) {
        (void)entryId;
        (void)position;
        return true;
    }
    /** After the session was disposed: hands the session tree to the next runtime (in-memory forks). */
    virtual std::unique_ptr<ISessionManager> releaseSessionManager() = 0;
};
