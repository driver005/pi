export module pi.testing.fake_server_listener;

import std;
export import pi.server.i_server_listener;
export import pi.testing.fake_byte_connection;

/** IServerListener whose connections are made by the test with connect(). */
export class FakeServerListener : public IServerListener {
public:
    Result<void> start(Acceptor accept) override;
    void close() override;

    /** A new client connection, already handed to the server; null before start() or after close(). */
    std::shared_ptr<FakeByteConnection> connect();
    bool started() const;
    void failStart(const Error& error);
    bool closedByServer() const;

private:
    mutable std::mutex m_mutex;
    Acceptor m_accept;
    std::vector<std::shared_ptr<FakeByteConnection>> m_connections;
    std::optional<Error> m_startFailure;
    bool m_closed = false;
};

Result<void> FakeServerListener::start(Acceptor accept) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_startFailure) {
        return std::unexpected(*m_startFailure);
    }
    m_accept = std::move(accept);
    return {};
}

void FakeServerListener::close() {
    std::vector<std::shared_ptr<FakeByteConnection>> connections;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        connections = m_connections;
    }
    for (const auto& connection : connections) {
        connection->close();
    }
}

std::shared_ptr<FakeByteConnection> FakeServerListener::connect() {
    Acceptor accept;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accept || m_closed) {
            return nullptr;
        }
        accept = m_accept;
    }
    auto connection = std::make_shared<FakeByteConnection>();
    connection->attach(accept(connection));
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_connections.push_back(connection);
    return connection;
}

bool FakeServerListener::started() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<bool>(m_accept);
}

void FakeServerListener::failStart(const Error& error) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_startFailure = error;
}

bool FakeServerListener::closedByServer() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}
