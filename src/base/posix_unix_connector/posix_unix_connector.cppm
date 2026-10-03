module;

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>

export module pi.base.posix_unix_connector;

import std;
export import pi.client.i_client_transport;
export import pi.server.i_socket_connection;
import pi.support.callback_connection_handler;

/**
 * Connects to a unix stream socket. Each connection gets a reader thread that feeds the handlers until the peer or close()
 * ends it; the destructor closes the connections still open and joins their threads.
 */
export class PosixUnixConnector : public IClientTransport {
public:
    /** Wraps a connected socket descriptor, taking ownership of it (PosixByteConnection in the composition root). */
    using Wrap = std::function<std::shared_ptr<ISocketConnection>(int fd, std::uint64_t maxPendingBytes, std::int64_t gracefulCloseTimeoutMs)>;

    PosixUnixConnector(std::string path, Wrap wrap, std::uint64_t maxPendingBytes = 64ULL * 1024 * 1024, std::int64_t gracefulCloseTimeoutMs = 1000)
        : m_path(std::move(path)),
          m_wrap(std::move(wrap)),
          m_maxPendingBytes(maxPendingBytes),
          m_gracefulCloseTimeoutMs(gracefulCloseTimeoutMs) {}

    ~PosixUnixConnector() override {
        for (const std::shared_ptr<ISocketConnection>& connection : m_connections) {
            connection->close();
        }
        for (std::thread& reader : m_readers) {
            reader.join();
        }
    }

    PosixUnixConnector(const PosixUnixConnector&) = delete;
    PosixUnixConnector& operator=(const PosixUnixConnector&) = delete;

    Result<std::shared_ptr<IByteConnection>> connect(ClientTransportHandlers handlers) override {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (m_path.size() >= sizeof(address.sun_path)) {
            return std::unexpected(Error{"unix_connect", "Unix socket path is too long: " + m_path});
        }
        std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::unexpected(Error{"unix_connect", std::string("Unable to create a socket: ") + std::strerror(errno)});
        }
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const std::string reason = std::strerror(errno);
            ::close(fd);
            return std::unexpected(Error{"unix_connect", "Unable to connect to " + m_path + ": " + reason});
        }
        const std::shared_ptr<ISocketConnection> connection = m_wrap(fd, m_maxPendingBytes, m_gracefulCloseTimeoutMs);
        const std::shared_ptr<IByteConnectionHandler> handler = std::make_shared<CallbackConnectionHandler>(std::move(handlers));
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.push_back(connection);
        m_readers.emplace_back([connection, handler] { connection->run(handler); });
        return std::shared_ptr<IByteConnection>(connection);
    }

private:
    std::string m_path;
    Wrap m_wrap;
    std::uint64_t m_maxPendingBytes;
    std::int64_t m_gracefulCloseTimeoutMs;
    std::mutex m_mutex;
    std::vector<std::shared_ptr<ISocketConnection>> m_connections;
    std::vector<std::thread> m_readers;
};
