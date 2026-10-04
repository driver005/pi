export module pi.types.server_connection_options;

import std;
export import pi.platform.i_executor;
export import pi.server.i_server_host;
export import pi.support.session_router;
export import pi.types.error;

/** What one server connection needs from its server. Callbacks may be called from any thread. */
export struct ServerConnectionOptions {
    IServerHost* host = nullptr;
    SessionRouter* router = nullptr;
    /** Runs request handling and disconnect cleanup off the connection's reader thread. */
    IExecutor* executor = nullptr;
    std::string serverId;
    /** Identifies this connection to the SessionRouter. */
    std::uint64_t clientId = 0;
    std::uint64_t maxFrameLength = 16ULL * 1024 * 1024;
    std::int64_t handshakeTimeoutMs = 5000;
    std::function<bool()> isClosing;
    std::function<void(const Error& error)> reportError;
    /** Called once when the connection is gone. */
    std::function<void(std::uint64_t clientId)> onDisconnect;
};
