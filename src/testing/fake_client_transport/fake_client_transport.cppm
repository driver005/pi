export module pi.testing.fake_client_transport;

import std;
export import pi.client.i_client_transport;
export import pi.support.protocol_codec;
export import pi.support.protocol_message_decoder;

/**
 * The server end of an in-memory client transport for client tests: the messages the client sends are decoded and
 * recorded (and offered to an optional responder that may answer from the sending thread), push() plays server
 * messages into the client's handlers, and remoteClose() ends the connection the way a dying server would.
 */
export class FakeClientTransport : public IClientTransport, public IByteConnection {
public:
    using Responder = std::function<void(const Json& clientMessage)>;

    FakeClientTransport()
        : m_decoder(ProtocolSide::Client) {}

    Result<std::shared_ptr<IByteConnection>> connect(ClientTransportHandlers handlers) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_refuse) {
            return std::unexpected(Error{"unix_connect", "connection refused"});
        }
        m_handlers = std::move(handlers);
        m_closed = false;
        ++m_connects;
        // A non-owning handle: the transport outlives the client in tests.
        return std::shared_ptr<IByteConnection>(std::shared_ptr<void>(), static_cast<IByteConnection*>(this));
    }

    bool closed() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    Result<void> send(std::string_view chunk) override {
        std::vector<Json> messages;
        Responder responder;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) {
                return std::unexpected(Error{"connection_closed", "Connection is closed"});
            }
            auto decoded = m_decoder.push(chunk);
            if (!decoded) {
                ++m_decodeFailures;
                return {};
            }
            messages = std::move(*decoded);
            for (const Json& message : messages) {
                m_received.push_back(message);
            }
            responder = m_responder;
        }
        m_changed.notify_all();
        for (const Json& message : messages) {
            if (responder) {
                responder(message);
            }
        }
        return {};
    }

    void close(std::string_view = {}) override {
        std::function<void()> onClose;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) {
                return;
            }
            m_closed = true;
            onClose = m_handlers.onClose;
        }
        m_changed.notify_all();
        if (onClose) {
            onClose();
        }
    }

    /** Answers every client message from the thread that sent it. */
    void respondWith(Responder responder) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_responder = std::move(responder);
    }

    /** Refuses the next connect(). */
    void refuseConnections() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_refuse = true;
    }

    /** Plays one server message into the client. */
    void push(const Json& message) {
        auto frame = m_codec.encode(ProtocolSide::Server, message);
        if (!frame) {
            return;
        }
        std::function<void(std::string_view)> onData;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            onData = m_handlers.onData;
        }
        if (onData) {
            onData(*frame);
        }
    }

    /** Plays raw bytes into the client. */
    void pushBytes(std::string_view bytes) {
        std::function<void(std::string_view)> onData;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            onData = m_handlers.onData;
        }
        if (onData) {
            onData(bytes);
        }
    }

    /** The server hanging up. */
    void remoteClose() {
        close();
    }

    /** Waits until `count` client messages arrived; returns them all. */
    std::vector<Json> waitForMessages(std::size_t count, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait_for(lock, timeout, [&] { return m_received.size() >= count; });
        return m_received;
    }

    std::vector<Json> received() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_received;
    }

    int connects() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_connects;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    ProtocolMessageDecoder m_decoder;
    ProtocolCodec m_codec;
    ClientTransportHandlers m_handlers;
    Responder m_responder;
    std::vector<Json> m_received;
    int m_connects = 0;
    int m_decodeFailures = 0;
    bool m_closed = true;
    bool m_refuse = false;
};
