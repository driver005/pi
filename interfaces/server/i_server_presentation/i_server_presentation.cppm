export module pi.server.i_server_presentation;

import std;
export import pi.types.result;
export import pi.types.service_context;

/** What a server-wide service may do for the connection it serves: pick and drop its session. */
export class IServerPresentation {
public:
    virtual ~IServerPresentation() = default;

    virtual Result<void> attachSession(const std::string& sessionId, const ServiceContext& context) = 0;
    virtual Result<void> detachSession(const ServiceContext& context) = 0;
    /** Releases routed attachments and handles before the application deletes the session's metadata. */
    virtual Result<void> prepareSessionRemoval(const std::string& sessionId, const ServiceContext& context) = 0;
};
