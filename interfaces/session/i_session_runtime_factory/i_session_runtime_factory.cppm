export module pi.session.i_session_runtime_factory;

import std;
export import pi.session.i_session_runtime_handle;
export import pi.types.result;
export import pi.types.session_runtime_request;

/**
 * Builds a session for a cwd and session tree: settings and resources for that cwd, the tool set,
 * the model runtime and the AgentSession over them. The composition root implements it.
 */
export class ISessionRuntimeFactory {
public:
    virtual ~ISessionRuntimeFactory() = default;

    virtual Result<std::unique_ptr<ISessionRuntimeHandle>> create(SessionRuntimeRequest request) = 0;
};
