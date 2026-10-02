export module pi.testing.scripted_session_state;

import std;
export import pi.server.i_routed_session_handle;
export import pi.testing.scripted_service_attachment;

/** What a test scripts and observes about one hosted session. */
export struct ScriptedSessionState {
    std::string id;
    ScriptedServiceAttachment::Handler handler;
    std::atomic<int> opened{0};
    std::atomic<int> attached{0};
    std::atomic<int> released{0};
    std::atomic<int> closed{0};
    std::optional<Error> openFailure;
    std::optional<Error> closeFailure;
    /** Set by the router; call it to end the session on its own. */
    std::mutex mutex;
    IRoutedSessionHandle::TerminationListener listener;
};
