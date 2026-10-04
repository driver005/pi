export module pi.types.server_options;

import std;
export import pi.types.error;

/** What a Server is configured with. `serverId` is the installation's stable logical identity. */
export struct ServerOptions {
    std::string serverId;
    std::uint64_t maxFrameLength = 16ULL * 1024 * 1024;
    std::int64_t handshakeTimeoutMs = 5000;
    std::function<void(std::size_t count)> onConnectionCountChanged;
    std::function<void(const Error& error)> onError;
};
