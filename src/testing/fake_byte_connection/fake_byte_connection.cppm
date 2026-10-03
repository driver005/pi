export module pi.testing.fake_byte_connection;

import std;
export import pi.server.i_byte_connection;
export import pi.server.i_byte_connection_handler;
export import pi.support.protocol_message_decoder;
export import pi.types.json;

/**
 * The client end of an in-memory connection for server tests: whatever the server sends is decoded
 * into protocol messages; feed() plays client messages into the server's handler.
 */
export class FakeByteConnection : public IByteConnection {
public:
    FakeByteConnection();

    bool closed() const override;
    Result<void> send(std::string_view chunk) override;
    void close(std::string_view finalChunk = {}) override;

    /** Where the server's reader thread would deliver bytes; set once the server accepted this connection. */
    void attach(std::shared_ptr<IByteConnectionHandler> handler);
    /** Encodes one client message and delivers it. */
    void feed(const Json& message);
    void feedRaw(std::string_view bytes);
    /** The peer hangs up. */
    void hangUp();
    void failSends(bool fail);

    std::vector<Json> messages() const;
    /** Waits until at least `count` server messages arrived. */
    bool waitForMessages(std::size_t count, std::int64_t timeoutMs = 3000) const;
    bool waitClosed(std::int64_t timeoutMs = 3000) const;
    std::size_t decodeFailures() const;

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::shared_ptr<IByteConnectionHandler> m_handler;
    ProtocolMessageDecoder m_decoder;
    ProtocolCodec m_codec;
    std::vector<Json> m_messages;
    std::size_t m_decodeFailures = 0;
    bool m_closed = false;
    bool m_failSends = false;
};

FakeByteConnection::FakeByteConnection() : m_decoder(ProtocolSide::Server) {}

bool FakeByteConnection::closed() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

Result<void> FakeByteConnection::send(std::string_view chunk) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed || m_failSends) {
        return std::unexpected(Error{"connection_closed", "Connection is closed"});
    }
    auto messages = m_decoder.push(chunk);
    if (!messages) {
        ++m_decodeFailures;
    } else {
        for (Json& message : *messages) {
            m_messages.push_back(std::move(message));
        }
    }
    m_changed.notify_all();
    return {};
}

void FakeByteConnection::close(std::string_view finalChunk) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed) {
        return;
    }
    if (!finalChunk.empty()) {
        auto messages = m_decoder.push(finalChunk);
        if (!messages) {
            ++m_decodeFailures;
        } else {
            for (Json& message : *messages) {
                m_messages.push_back(std::move(message));
            }
        }
    }
    m_closed = true;
    m_changed.notify_all();
}

void FakeByteConnection::attach(std::shared_ptr<IByteConnectionHandler> handler) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_handler = std::move(handler);
}

void FakeByteConnection::feed(const Json& message) {
    auto frame = m_codec.encode(ProtocolSide::Client, message);
    if (frame) {
        feedRaw(*frame);
    }
}

void FakeByteConnection::feedRaw(std::string_view bytes) {
    std::shared_ptr<IByteConnectionHandler> handler;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        handler = m_handler;
    }
    if (handler) {
        handler->onData(bytes);
    }
}

void FakeByteConnection::hangUp() {
    std::shared_ptr<IByteConnectionHandler> handler;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        handler = m_handler;
        m_changed.notify_all();
    }
    if (handler) {
        handler->onClose();
    }
}

void FakeByteConnection::failSends(bool fail) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_failSends = fail;
}

std::vector<Json> FakeByteConnection::messages() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_messages;
}

bool FakeByteConnection::waitForMessages(std::size_t count, std::int64_t timeoutMs) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return m_messages.size() >= count; });
}

bool FakeByteConnection::waitClosed(std::int64_t timeoutMs) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return m_closed; });
}

std::size_t FakeByteConnection::decodeFailures() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_decodeFailures;
}
