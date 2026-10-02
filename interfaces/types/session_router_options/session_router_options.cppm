export module pi.types.session_router_options;

import std;
export import pi.platform.i_executor;
export import pi.platform.i_id_generator;
export import pi.server.i_server_host;
export import pi.types.error;
export import pi.types.service_context;
export import pi.types.session_attachment;

/** What a SessionRouter works with. Callbacks may be called from any thread. */
export struct SessionRouterOptions {
    IServerHost* host = nullptr;
    IIdGenerator* ids = nullptr;
    /** Runs cleanup that must not block the caller (a session ending on its own). */
    IExecutor* executor = nullptr;
    std::string serverId;
    std::function<bool()> isClosing;
    /** Tells a client which session it is attached to, or none. */
    std::function<void(std::uint64_t client, const std::optional<SessionAttachment>& attachment,
                       const ServiceContext& context)>
        publishAttachment;
    std::function<void(const Error& error)> reportError;
};
