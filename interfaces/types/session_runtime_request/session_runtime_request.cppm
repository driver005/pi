export module pi.types.session_runtime_request;

import std;
export import pi.session.i_session_manager;

/** What a runtime factory builds a session around. */
export struct SessionRuntimeRequest {
    std::string cwd;
    std::string agentDir;
    std::unique_ptr<ISessionManager> sessionManager;
    /** Why the session starts: "startup", "new", "resume", "fork" or "reload". */
    std::string startReason = "startup";
    std::optional<std::string> previousSessionFile;
};
