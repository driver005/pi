export module pi.session.i_agent_session_runtime;

import std;
export import pi.session.i_agent_session;
export import pi.types.fork_result;
export import pi.types.result;
export import pi.types.runtime_diagnostic;

/**
 * Owns the current session and replaces it: a new session, another session file, a fork of the
 * current one or an imported file. Replacing stops and disposes the old session first. The
 * session reference changes after a replacement, so clients re-read it. Port of AgentSessionRuntime.
 */
export class IAgentSessionRuntime {
public:
    virtual ~IAgentSessionRuntime() = default;

    virtual IAgentSession& session() = 0;
    virtual std::string cwd() const = 0;
    virtual std::vector<RuntimeDiagnostic> diagnostics() const = 0;

    virtual Result<void> newSession(const std::optional<std::string>& parentSession) = 0;
    virtual Result<void> switchSession(const std::string& sessionPath, const std::optional<std::string>& cwdOverride) = 0;
    virtual Result<ForkResult> fork(const std::string& entryId, ForkPosition position) = 0;
    /** Copies a session JSONL file into the session directory and switches to it. */
    virtual Result<void> importFromJsonl(const std::string& inputPath, const std::optional<std::string>& cwdOverride) = 0;
    virtual void dispose() = 0;
};
