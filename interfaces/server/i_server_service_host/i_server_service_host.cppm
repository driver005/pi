export module pi.server.i_server_service_host;

import std;
export import pi.server.i_server_presentation;
export import pi.server.i_service_attachment;
export import pi.types.result;
export import pi.types.service_context;

/** The server-wide services (session directory, session management, ...). */
export class IServerServiceHost {
public:
    virtual ~IServerServiceHost() = default;

    /** `presentation` stays valid until the returned attachment is released. */
    virtual Result<std::unique_ptr<IServiceAttachment>> attachClient(IServerPresentation& presentation,
                                                                     const ServiceContext& context) = 0;
};
