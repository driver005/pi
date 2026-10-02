module;

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>

export module pi.base.posix_unix_listener;

import std;
export import pi.server.i_server_listener;
export import pi.types.unix_listener_options;

/**
 * IServerListener on a unix-domain socket. Start replaces a stale socket file (never a live one or
 * a non-socket), binds a private temporary path and links it to the public path so the socket is
 * never visible with the wrong mode, and accepts only connections of the same user. Close removes
 * the socket file it created, but only if it is still the same file. Port of
 * packages/server/src/transports/unix/listener.ts.
 */
export class PosixUnixListener : public IServerListener {
public:
    explicit PosixUnixListener(UnixListenerOptions options);
    ~PosixUnixListener() override;

    PosixUnixListener(const PosixUnixListener&) = delete;
    PosixUnixListener& operator=(const PosixUnixListener&) = delete;

    Result<void> start(Acceptor accept) override;
    void close() override;

private:
    Result<void> prepareDirectory() const;
    Result<void> removeStale(const std::string& path) const;
    bool isLive(const std::string& path) const;
    Result<int> bindAndListen(const std::string& bindPath) const;
    Result<void> publish(const std::string& bindPath);
    void acceptLoop();
    void serve(int fd);
    bool authorized(int fd) const;
    void removeOwnedSocket();
    void report(const Error& error) const;
    std::string bindPath() const;
    Error failure(const std::string& what) const;
    Result<sockaddr_un> address(const std::string& path) const;

    UnixListenerOptions m_options;
    Acceptor m_accept;
    int m_listenFd = -1;
    std::array<int, 2> m_wake{-1, -1};
    std::thread m_acceptThread;
    dev_t m_socketDevice = 0;
    ino_t m_socketInode = 0;
    bool m_published = false;

    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::set<std::shared_ptr<ISocketConnection>> m_connections;
    int m_active = 0;
    bool m_started = false;
    bool m_closing = false;
    bool m_closed = false;
};

PosixUnixListener::PosixUnixListener(UnixListenerOptions options) : m_options(std::move(options)) {}

PosixUnixListener::~PosixUnixListener() {
    close();
}

void PosixUnixListener::report(const Error& error) const {
    if (m_options.onError) {
        m_options.onError(error);
    }
}

Error PosixUnixListener::failure(const std::string& what) const {
    return Error{"unix_listener", what + ": " + std::strerror(errno)};
}

std::string PosixUnixListener::bindPath() const {
    const std::filesystem::path path(m_options.path);
    const std::size_t hash = std::hash<std::string>{}(m_options.path);
    return (path.parent_path() / std::format("bind-{:08x}", static_cast<std::uint32_t>(hash))).string();
}

Result<sockaddr_un> PosixUnixListener::address(const std::string& path) const {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    if (path.size() >= sizeof(result.sun_path)) {
        return std::unexpected(Error{"unix_listener", "Unix socket path is too long: " + path});
    }
    std::memcpy(result.sun_path, path.c_str(), path.size() + 1);
    return result;
}

Result<void> PosixUnixListener::prepareDirectory() const {
    const std::filesystem::path directory = std::filesystem::path(m_options.path).parent_path();
    std::error_code ec;
    const bool existed = std::filesystem::exists(directory, ec);
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return std::unexpected(Error{"unix_listener", "Unable to create " + directory.string() + ": " + ec.message()});
    }
    if (!existed) {
        ::chmod(directory.c_str(), 0700);
    }
    return {};
}

bool PosixUnixListener::isLive(const std::string& path) const {
    auto target = address(path);
    if (!target) {
        return true;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return true;
    }
    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&*target), sizeof(*target));
    const int error = errno;
    ::close(fd);
    return rc == 0 || (error != ECONNREFUSED && error != ENOENT);
}

Result<void> PosixUnixListener::removeStale(const std::string& path) const {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        return errno == ENOENT ? Result<void>() : std::unexpected(failure("Unable to inspect " + path));
    }
    if (!S_ISSOCK(info.st_mode)) {
        return std::unexpected(Error{"unix_listener", "Refusing to remove non-socket Unix listener path: " + path});
    }
    if (isLive(path)) {
        return std::unexpected(Error{"unix_listener", "Unix listener is already running: " + path});
    }
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        return std::unexpected(failure("Unable to remove stale socket " + path));
    }
    return {};
}

Result<int> PosixUnixListener::bindAndListen(const std::string& path) const {
    auto target = address(path);
    if (!target) {
        return std::unexpected(target.error());
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(failure("Unable to create socket"));
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&*target), sizeof(*target)) != 0) {
        const Error error = failure("Unable to bind " + path);
        ::close(fd);
        return std::unexpected(error);
    }
    if (::chmod(path.c_str(), m_options.mode) != 0 || ::listen(fd, 128) != 0) {
        const Error error = failure("Unable to listen on " + path);
        ::close(fd);
        ::unlink(path.c_str());
        return std::unexpected(error);
    }
    return fd;
}

Result<void> PosixUnixListener::publish(const std::string& path) {
    if (::link(path.c_str(), m_options.path.c_str()) != 0) {
        const Error error = errno == EEXIST ? Error{"unix_listener", "Unix listener is already running: " + m_options.path}
                                            : failure("Unable to publish " + m_options.path);
        return std::unexpected(error);
    }
    m_published = true;
    struct stat info {};
    if (::lstat(m_options.path.c_str(), &info) == 0) {
        m_socketDevice = info.st_dev;
        m_socketInode = info.st_ino;
    }
    return {};
}

Result<void> PosixUnixListener::start(Acceptor accept) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started) {
        return std::unexpected(Error{"unix_listener", "Unix listener is already started"});
    }
    if (m_closing || m_closed) {
        return std::unexpected(Error{"unix_listener", "Unix listener is closing or closed"});
    }
    if (!m_options.connect) {
        return std::unexpected(Error{"invalid_options", "Unix listener needs a connection factory"});
    }
    if (auto fits = address(m_options.path); !fits) {
        return std::unexpected(fits.error());
    }
    const std::string temporary = bindPath();
    if (auto prepared = prepareDirectory(); !prepared) {
        return prepared;
    }
    if (auto stale = removeStale(m_options.path); !stale) {
        return stale;
    }
    if (auto stale = removeStale(temporary); !stale) {
        return stale;
    }
    auto listening = bindAndListen(temporary);
    if (!listening) {
        return std::unexpected(listening.error());
    }
    m_listenFd = *listening;
    auto published = publish(temporary);
    ::unlink(temporary.c_str());
    if (!published || ::pipe(m_wake.data()) != 0) {
        const Error error = published ? failure("Unable to create wake pipe") : published.error();
        removeOwnedSocket();
        ::close(m_listenFd);
        m_listenFd = -1;
        return std::unexpected(error);
    }
    m_accept = std::move(accept);
    m_started = true;
    m_acceptThread = std::thread([this] { acceptLoop(); });
    return {};
}

bool PosixUnixListener::authorized(int fd) const {
#ifdef __linux__
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 && credentials.uid == ::geteuid();
#else
    uid_t uid = 0;
    gid_t gid = 0;
    return ::getpeereid(fd, &uid, &gid) == 0 && uid == ::geteuid();
#endif
}

void PosixUnixListener::acceptLoop() {
    while (true) {
        std::array<pollfd, 2> fds{{{m_listenFd, POLLIN, 0}, {m_wake[0], POLLIN, 0}}};
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            report(failure("poll failed"));
            return;
        }
        if (fds[1].revents != 0) {
            return;
        }
        if (fds[0].revents == 0) {
            continue;
        }
        const int fd = ::accept(m_listenFd, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) {
                report(failure("accept failed"));
            }
            continue;
        }
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        serve(fd);
    }
}

void PosixUnixListener::serve(int fd) {
    if (!authorized(fd)) {
        ::close(fd);
        return;
    }
    auto connection = m_options.connect(fd, m_options.maxPendingBytes, m_options.gracefulCloseTimeoutMs);
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closing) {
            connection->close();
            return;
        }
        m_connections.insert(connection);
        ++m_active;
    }
    std::thread([this, connection, handler = m_accept(connection)]() mutable {
        connection->run(handler);
        handler.reset();
        const std::shared_ptr<ISocketConnection> finished = std::move(connection);
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.erase(finished);
        --m_active;
        m_changed.notify_all();
    }).detach();
}

void PosixUnixListener::removeOwnedSocket() {
    if (!m_published) {
        return;
    }
    m_published = false;
    struct stat info {};
    if (::lstat(m_options.path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) && info.st_dev == m_socketDevice &&
        info.st_ino == m_socketInode) {
        ::unlink(m_options.path.c_str());
    }
}

void PosixUnixListener::close() {
    std::vector<std::shared_ptr<ISocketConnection>> connections;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || m_closing) {
            return;
        }
        m_closing = true;
    }
    if (m_started) {
        const char wake = 1;
        if (::write(m_wake[1], &wake, 1) < 0) {
            report(failure("Unable to wake the accept loop"));
        }
        m_acceptThread.join();
        ::close(m_listenFd);
        ::close(m_wake[0]);
        ::close(m_wake[1]);
        removeOwnedSocket();
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    connections.assign(m_connections.begin(), m_connections.end());
    lock.unlock();
    for (const auto& connection : connections) {
        connection->close();
    }
    lock.lock();
    m_changed.wait(lock, [&] { return m_active == 0; });
    m_closed = true;
}
