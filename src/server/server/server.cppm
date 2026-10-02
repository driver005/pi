module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.server.server;

import std;
export import pi.platform.i_executor;
export import pi.platform.i_id_generator;
export import pi.server.i_server;
export import pi.server.i_server_host;
export import pi.server.i_server_listener;
export import pi.support.protocol_codec;
export import pi.support.server_connection;
export import pi.support.session_router;
export import pi.types.server_options;

/**
 * The protocol server: accepts connections from its listeners, runs one ServerConnection for each,
 * routes sessions through a SessionRouter, enforces the handshake timeout and drains on close.
 * Port of packages/server/src/server.ts.
 */
export class Server : public IServer {
public:
    Server(IServerHost& host, IExecutor& executor, IIdGenerator& ids, std::vector<IServerListener*> listeners,
           ServerOptions options);
    ~Server() override;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    const std::string& serverId() const override;
    Result<void> start() override;
    Result<void> close() override;

private:
    std::shared_ptr<IByteConnectionHandler> accept(const std::shared_ptr<IByteConnection>& connection);
    Result<void> validateOptions() const;
    void removeConnection(std::uint64_t clientId);
    void sendAttachment(std::uint64_t clientId, const std::optional<SessionAttachment>& attachment);
    void watch(const std::stop_token& stop);
    void notifyCount(std::size_t count);
    void report(const Error& error);
    ServiceContext background() const;
    Result<void> closeNow();

    IServerHost& m_host;
    IExecutor& m_executor;
    std::vector<IServerListener*> m_listeners;
    ServerOptions m_options;
    SessionRouter m_router;
    ProtocolCodec m_codec;

    std::atomic<bool> m_closing{false};
    std::mutex m_mutex;
    std::map<std::uint64_t, std::shared_ptr<ServerConnection>> m_connections;
    std::uint64_t m_nextClient = 1;
    bool m_started = false;

    std::mutex m_lifecycleMutex;
    std::optional<Result<void>> m_closeResult;
    std::mutex m_watchMutex;
    std::condition_variable_any m_watchWake;
    std::jthread m_watchdog;
};

Server::Server(IServerHost& host, IExecutor& executor, IIdGenerator& ids, std::vector<IServerListener*> listeners,
               ServerOptions options)
    : m_host(host),
      m_executor(executor),
      m_listeners(std::move(listeners)),
      m_options(std::move(options)),
      m_router([&] {
          SessionRouterOptions routerOptions;
          routerOptions.host = &host;
          routerOptions.ids = &ids;
          routerOptions.executor = &executor;
          routerOptions.serverId = m_options.serverId;
          routerOptions.isClosing = [this] { return m_closing.load(); };
          routerOptions.publishAttachment = [this](std::uint64_t client, const std::optional<SessionAttachment>& attachment,
                                                   const ServiceContext&) { sendAttachment(client, attachment); };
          routerOptions.reportError = [this](const Error& error) { report(error); };
          return routerOptions;
      }()) {}

Server::~Server() {
    close();
}

const std::string& Server::serverId() const {
    return m_options.serverId;
}

ServiceContext Server::background() const {
    return ServiceContext{std::make_shared<AbortSignal>()};
}

void Server::report(const Error& error) {
    if (m_options.onError) {
        m_options.onError(error);
    }
}

void Server::notifyCount(std::size_t count) {
    if (m_options.onConnectionCountChanged) {
        m_options.onConnectionCountChanged(count);
    }
}

Result<void> Server::validateOptions() const {
    const Json hello = {{"type", "hello"}, {"version", ProtocolValidator::kProtocolVersion}, {"serverId", m_options.serverId}};
    if (!m_codec.encode(ProtocolSide::Server, hello)) {
        return std::unexpected(Error{"invalid_options", "serverId must be a canonical lowercase UUIDv4"});
    }
    if (m_options.maxFrameLength == 0 || m_options.maxFrameLength > 0xffffffffULL) {
        return std::unexpected(Error{"invalid_options", "Server maxFrameLength must be an integer between 1 and 4294967295"});
    }
    if (m_options.handshakeTimeoutMs <= 0) {
        return std::unexpected(Error{"invalid_options", "Server handshakeTimeoutMs must be a positive integer"});
    }
    return {};
}

Result<void> Server::start() {
    const std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    if (m_started) {
        return std::unexpected(Error{"invalid_state", "Server is already started"});
    }
    if (m_closing.load()) {
        return std::unexpected(Error{"invalid_state", "Server is closing or closed"});
    }
    if (auto valid = validateOptions(); !valid) {
        return valid;
    }
    std::vector<IServerListener*> started;
    for (IServerListener* listener : m_listeners) {
        auto result = listener->start([this](std::shared_ptr<IByteConnection> connection) { return accept(connection); });
        if (!result) {
            m_closing = true;
            for (IServerListener* done : started) {
                done->close();
            }
            m_router.close(background());
            return result;
        }
        started.push_back(listener);
    }
    m_started = true;
    m_watchdog = std::jthread([this](std::stop_token stop) { watch(stop); });
    return {};
}

std::shared_ptr<IByteConnectionHandler> Server::accept(const std::shared_ptr<IByteConnection>& connection) {
    ServerConnectionOptions options;
    options.host = &m_host;
    options.router = &m_router;
    options.executor = &m_executor;
    options.serverId = m_options.serverId;
    options.maxFrameLength = m_options.maxFrameLength;
    options.handshakeTimeoutMs = m_options.handshakeTimeoutMs;
    options.isClosing = [this] { return m_closing.load(); };
    options.reportError = [this](const Error& error) { report(error); };
    options.onDisconnect = [this](std::uint64_t clientId) { removeConnection(clientId); };
    std::shared_ptr<ServerConnection> created;
    std::size_t count = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        options.clientId = m_nextClient++;
        created = std::make_shared<ServerConnection>(connection, options);
        m_connections[options.clientId] = created;
        count = m_connections.size();
    }
    notifyCount(count);
    if (m_closing.load()) {
        created->shutdown();
    }
    return created;
}

void Server::removeConnection(std::uint64_t clientId) {
    std::size_t count = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_connections.erase(clientId) == 0) {
            return;
        }
        count = m_connections.size();
    }
    notifyCount(count);
}

void Server::sendAttachment(std::uint64_t clientId, const std::optional<SessionAttachment>& attachment) {
    std::shared_ptr<ServerConnection> connection;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_connections.find(clientId);
        if (found == m_connections.end()) {
            return;
        }
        connection = found->second;
    }
    connection->sendAttachment(attachment);
}

void Server::watch(const std::stop_token& stop) {
    const std::int64_t pollMs = std::clamp<std::int64_t>(m_options.handshakeTimeoutMs / 4, 1, 100);
    while (!stop.stop_requested()) {
        std::vector<std::shared_ptr<ServerConnection>> connections;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& entry : m_connections) {
                connections.push_back(entry.second);
            }
        }
        for (const auto& connection : connections) {
            connection->checkHandshakeTimeout();
        }
        std::unique_lock<std::mutex> lock(m_watchMutex);
        m_watchWake.wait_for(lock, stop, std::chrono::milliseconds(pollMs), [] { return false; });
    }
}

Result<void> Server::close() {
    m_closing = true;
    const std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    if (!m_closeResult) {
        m_closeResult = closeNow();
    }
    return *m_closeResult;
}

Result<void> Server::closeNow() {
    for (IServerListener* listener : m_listeners) {
        listener->close();
    }
    std::vector<std::shared_ptr<ServerConnection>> connections;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_connections) {
            connections.push_back(entry.second);
        }
    }
    for (const auto& connection : connections) {
        connection->shutdown();
    }
    if (m_watchdog.joinable()) {
        m_watchdog.request_stop();
        m_watchdog.join();
    }
    m_started = false;
    return m_router.close(background());
}
