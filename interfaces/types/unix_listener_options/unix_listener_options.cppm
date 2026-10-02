export module pi.types.unix_listener_options;

import std;
export import pi.server.i_socket_connection;
export import pi.types.error;

/** Where a unix-socket listener binds and how it serves. */
export struct UnixListenerOptions {
    /** The socket file, normally `<serverDirectory>/<serverId>.sock`. */
    std::string path;
    /** File mode of the socket; owner read/write only by default. */
    unsigned mode = 0600;
    /** Bytes queued per connection before a slow peer is disconnected. */
    std::uint64_t maxPendingBytes = 64ULL * 1024 * 1024;
    /** How long a closing connection waits for its peer to hang up before it is cut. */
    std::int64_t gracefulCloseTimeoutMs = 5000;
    /** Wraps an accepted socket descriptor, taking ownership of it. */
    std::function<std::shared_ptr<ISocketConnection>(int fd, std::uint64_t maxPendingBytes,
                                                     std::int64_t gracefulCloseTimeoutMs)>
        connect;
    std::function<void(const Error& error)> onError;
};
