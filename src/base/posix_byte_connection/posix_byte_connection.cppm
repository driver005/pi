module;

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

export module pi.base.posix_byte_connection;

import std;
export import pi.server.i_socket_connection;
export import pi.types.result;

/**
 * An accepted stream socket. send() queues bytes for a writer thread (and fails once the queue
 * exceeds the pending limit, which disconnects slow peers); close() flushes the queue and an optional
 * final chunk, half-closes, then waits for the peer to hang up before cutting the connection.
 * run() is the read loop, executed on a thread the owner provides.
 */
export class PosixByteConnection : public ISocketConnection {
public:
    PosixByteConnection(int fd, std::uint64_t maxPendingBytes, std::int64_t gracefulCloseTimeoutMs);
    ~PosixByteConnection() override;

    PosixByteConnection(const PosixByteConnection&) = delete;
    PosixByteConnection& operator=(const PosixByteConnection&) = delete;

    bool closed() const override;
    Result<void> send(std::string_view chunk) override;
    void close(std::string_view finalChunk = {}) override;
    void run(std::shared_ptr<IByteConnectionHandler> handler) override;

private:
    void writeLoop();
    bool writeAll(const std::string& bytes);
    void markClosed();

    int m_fd;
    std::uint64_t m_maxPendingBytes;
    std::int64_t m_gracefulCloseTimeoutMs;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::string> m_queue;
    std::uint64_t m_pendingBytes = 0;
    bool m_closing = false;
    bool m_closed = false;
    std::thread m_writer;
};

PosixByteConnection::PosixByteConnection(int fd, std::uint64_t maxPendingBytes, std::int64_t gracefulCloseTimeoutMs)
    : m_fd(fd), m_maxPendingBytes(maxPendingBytes), m_gracefulCloseTimeoutMs(gracefulCloseTimeoutMs) {
    m_writer = std::thread([this] { writeLoop(); });
}

PosixByteConnection::~PosixByteConnection() {
    markClosed();
    if (m_writer.joinable()) {
        m_writer.join();
    }
    ::close(m_fd);
}

bool PosixByteConnection::closed() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed || m_closing;
}

Result<void> PosixByteConnection::send(std::string_view chunk) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || m_closing) {
            return std::unexpected(Error{"connection_closed", "Unix connection is closed"});
        }
        if (m_pendingBytes + chunk.size() > m_maxPendingBytes) {
            return std::unexpected(Error{"connection_slow", "Unix connection exceeded its pending byte limit"});
        }
        m_pendingBytes += chunk.size();
        m_queue.emplace_back(chunk);
    }
    m_wake.notify_all();
    return {};
}

void PosixByteConnection::close(std::string_view finalChunk) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || m_closing) {
            return;
        }
        m_closing = true;
        if (!finalChunk.empty()) {
            m_pendingBytes += finalChunk.size();
            m_queue.emplace_back(finalChunk);
        }
    }
    m_wake.notify_all();
}

void PosixByteConnection::markClosed() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_queue.clear();
    }
    ::shutdown(m_fd, SHUT_RDWR);
    m_wake.notify_all();
}

bool PosixByteConnection::writeAll(const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::send(m_fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

void PosixByteConnection::writeLoop() {
    while (true) {
        std::string chunk;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&] { return !m_queue.empty() || m_closing || m_closed; });
            if (m_closed) {
                return;
            }
            if (m_queue.empty()) {
                break;
            }
            chunk = std::move(m_queue.front());
            m_queue.pop_front();
        }
        if (!writeAll(chunk)) {
            markClosed();
            return;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingBytes -= chunk.size();
    }
    ::shutdown(m_fd, SHUT_WR);
    std::unique_lock<std::mutex> lock(m_mutex);
    m_wake.wait_for(lock, std::chrono::milliseconds(m_gracefulCloseTimeoutMs), [&] { return m_closed; });
    if (!m_closed) {
        ::shutdown(m_fd, SHUT_RDWR);
    }
}

void PosixByteConnection::run(std::shared_ptr<IByteConnectionHandler> handler) {
    std::array<char, 65536> buffer{};
    while (true) {
        const ssize_t received = ::recv(m_fd, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            handler->onData(std::string_view(buffer.data(), static_cast<std::size_t>(received)));
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && !closed()) {
            handler->onError(Error{"socket_error", std::strerror(errno)});
        }
        break;
    }
    markClosed();
    handler->onClose();
}
