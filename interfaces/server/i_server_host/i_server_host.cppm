export module pi.server.i_server_host;

import std;
export import pi.server.i_routed_session_handle;
export import pi.server.i_server_service_host;
export import pi.types.result;
export import pi.types.service_context;

/** The application behind a server: its server-wide services and its sessions. */
export class IServerHost {
public:
    virtual ~IServerHost() = default;

    virtual IServerServiceHost& serverServices() = 0;
    /** The canonical id of the session an id or unambiguous prefix names; session_not_found / session_ambiguous otherwise. */
    virtual Result<std::string> resolveSession(const std::string& sessionId, const ServiceContext& context) = 0;
    virtual Result<std::shared_ptr<IRoutedSessionHandle>> openSession(const std::string& sessionId,
                                                                      const ServiceContext& context) = 0;
};
