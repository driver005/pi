module;

#include <cstdint>

export module pi.support.protocol_client;

import std;
export import pi.client.i_client_transport;
export import pi.support.abort_signal;
export import pi.support.protocol_codec;
export import pi.support.protocol_message_decoder;
export import pi.support.service_state_decoder;
export import pi.support.service_wire;
export import pi.types.client_subscription;
export import pi.types.client_subscription_state;
export import pi.types.pending_client_request;

/**
 * A client of a protocol server (protocol v8): connects over an IClientTransport, runs the hello handshake and then offers
 * blocking requests with cancellation, service catalogues and service subscriptions with decoded updates, and the session
 * attachment the server reports. Port of Client and Connection in packages/client.
 *
 * Calls may come from any thread. Responses are matched on the transport's reader thread; update listeners, the attachment
 * listener and the disconnect listener run in order on a delivery thread of the client, so they may call back into it.
 * A subscription's updates are held until start(), so the caller can install the snapshot first. A protocol violation or a
 * lost transport disconnects the client: pending requests fail with the code "disconnected" (or the violation's code), the
 * subscriptions end and the attachment clears. The destructor disconnects and waits for the transport's last callback.
 */
export class ProtocolClient {
public:
    using UpdateListener = std::function<void(const Json& update)>;
    using AttachmentListener = std::function<void(const Json& attachment)>;
    using DisconnectListener = std::function<void(const Error& error)>;

    ProtocolClient(IClientTransport& transport, std::string serverId, std::uint64_t maxFrameLength = FrameDecoder::kDefaultMaxFrameLength, std::int64_t handshakeTimeoutMs = 10000)
        : m_transport(transport),
          m_serverId(std::move(serverId)),
          m_maxFrameLength(maxFrameLength),
          m_handshakeTimeoutMs(handshakeTimeoutMs),
          m_decoder(ProtocolSide::Server, maxFrameLength) {
        m_delivery = std::thread([this] { deliveryLoop(); });
    }

    ~ProtocolClient() {
        disconnect("Client disposed");
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [&] { return !m_readerActive; });
        }
        {
            const std::lock_guard<std::mutex> lock(m_deliveryMutex);
            m_stopping = true;
        }
        m_deliveryWake.notify_all();
        m_delivery.join();
    }

    ProtocolClient(const ProtocolClient&) = delete;
    ProtocolClient& operator=(const ProtocolClient&) = delete;

    /** Connects and returns the server's hello `{type, version, serverId}`. */
    Result<Json> connect() {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_connecting || m_connected) {
            return std::unexpected(Error{"disconnected", m_connected ? "Client is already connected" : "Client is already connecting"});
        }
        m_changed.wait(lock, [&] { return !m_readerActive; });
        m_connecting = true;
        m_failure.reset();
        m_hello.reset();
        m_decoder = ProtocolMessageDecoder(ProtocolSide::Server, m_maxFrameLength);
        m_readerActive = true;
        lock.unlock();

        ClientTransportHandlers handlers;
        handlers.onData = [this](std::string_view chunk) { onData(chunk); };
        handlers.onClose = [this] { onClose(); };
        handlers.onError = [this](const Error& error) { fail(error); };
        auto connection = m_transport.connect(std::move(handlers));
        lock.lock();
        if (!connection) {
            m_connecting = false;
            m_readerActive = false;
            m_changed.notify_all();
            return std::unexpected(Error{"disconnected", connection.error().message});
        }
        m_connection = *connection;
        const std::shared_ptr<IByteConnection> active = m_connection;
        lock.unlock();

        auto frame = m_codec.encode(ProtocolSide::Client, Json{{"type", "hello"}, {"version", ProtocolValidator::kProtocolVersion}}, m_maxFrameLength);
        if (!frame || !active->send(*frame)) {
            fail(Error{"disconnected", frame ? "Unable to send the client hello" : frame.error().message});
        }
        lock.lock();
        const bool settled = m_changed.wait_for(lock, std::chrono::milliseconds(m_handshakeTimeoutMs), [&] { return !m_connecting; });
        if (!settled) {
            lock.unlock();
            fail(Error{"handshake_timeout", "The server did not answer the client hello"});
            lock.lock();
        }
        if (m_connected && m_hello) {
            return *m_hello;
        }
        return std::unexpected(m_failure ? *m_failure : Error{"disconnected", "Connection failed"});
    }

    void disconnect(const std::string& reason = "Client disconnected") {
        fail(Error{"disconnected", reason});
    }

    bool connected() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_connected;
    }

    std::optional<Json> hello() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_hello;
    }

    /** The session target the server attached this client to (`{serverId, sessionId, attachmentId}`), if any. */
    std::optional<Json> attachment() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_attachment;
    }

    /** Runs `listener` with the new attachment (null when it ended) whenever it changes. */
    void onAttachment(AttachmentListener listener) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_attachmentListeners.push_back(std::move(listener));
    }

    /** Runs `listener` once per connection, when it ends. */
    void onDisconnect(DisconnectListener listener) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_disconnectListener = std::move(listener);
    }

    /** The target of the server itself. */
    Json serverTarget() const {
        return Json{{"serverId", m_serverId}};
    }

    /**
     * One protocol call against a routed target (`serverTarget()` or an attachment). Blocks for the response; aborting
     * `signal` fails the call with the code "aborted" at once and tells the server to cancel it. A server error keeps the
     * server's code and message.
     */
    Result<std::optional<Json>> request(const Json& target, const Json& call, const std::shared_ptr<AbortSignal>& signal = nullptr) {
        auto result = send(target, call, signal, {});
        if (!result) {
            return std::unexpected(result.error());
        }
        return *result;
    }

    Result<Json> serviceCatalogue(const Json& target, const std::shared_ptr<AbortSignal>& signal = nullptr) {
        auto result = request(target, m_wire.catalogueCall(), signal);
        if (!result) {
            return std::unexpected(result.error());
        }
        const Json catalogue = result->value_or(Json());
        if (auto valid = m_wire.validateCatalogue(catalogue); !valid) {
            fail(valid.error());
            return std::unexpected(valid.error());
        }
        return catalogue;
    }

    /**
     * Subscribes to a service (`mode` "singleton" or "keyed"). The decoded baseline snapshot comes back; the updates after
     * it reach `listener` once start() is called. Updates are decoded ops (`state` updates carry full operations).
     */
    Result<ClientSubscription> subscribeService(const Json& target, const std::string& serviceId, const std::string& mode, UpdateListener listener, const std::shared_ptr<AbortSignal>& signal = nullptr) {
        std::string id;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            id = "service-" + std::to_string(++m_subscriptionSequence);
            ClientSubscriptionState state;
            state.target = target;
            state.listener = std::move(listener);
            m_subscriptions.emplace(id, std::move(state));
            m_decoders.emplace(id, ServiceStateDecoder());
        }
        auto result = send(target, m_wire.subscribeCall(id, serviceId, mode), signal, [this, id](const Json& wire) -> Result<Json> { return hydrate(id, wire); });
        if (!result) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_subscriptions.erase(id);
            m_decoders.erase(id);
            return std::unexpected(result.error());
        }
        ClientSubscription subscription;
        subscription.id = id;
        subscription.target = target;
        subscription.snapshot = result->value_or(Json());
        return subscription;
    }

    /** Begins ordered update delivery, after the caller installed the snapshot. */
    void start(const std::string& subscriptionId) {
        std::vector<Json> queued;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_subscriptions.find(subscriptionId);
            if (found == m_subscriptions.end() || found->second.ready) {
                return;
            }
            found->second.ready = true;
            queued.swap(found->second.queued);
        }
        for (Json& update : queued) {
            deliverUpdate(subscriptionId, std::move(update));
        }
    }

    /** Ends a subscription; no update reaches its listener afterwards. */
    Result<void> dispose(const std::string& subscriptionId) {
        Json target;
        bool wasConnected = false;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_subscriptions.find(subscriptionId);
            if (found == m_subscriptions.end()) {
                return {};
            }
            target = found->second.target;
            m_subscriptions.erase(found);
            m_decoders.erase(subscriptionId);
            wasConnected = m_connected;
        }
        if (!wasConnected) {
            return {};
        }
        auto answered = request(target, m_wire.unsubscribeCall(subscriptionId));
        return answered ? Result<void>() : std::unexpected(answered.error());
    }

private:
    using Transform = std::function<Result<Json>(const Json& result)>;

    Result<std::optional<Json>> send(const Json& target, const Json& call, const std::shared_ptr<AbortSignal>& signal, Transform transform) {
        if (signal && signal->aborted()) {
            return std::unexpected(Error{"aborted", "The operation was aborted"});
        }
        if (auto valid = m_wire.validateCall(call); !valid) {
            return std::unexpected(valid.error());
        }
        const auto entry = std::make_shared<PendingClientRequest>();
        entry->transform = std::move(transform);
        std::string id;
        std::shared_ptr<IByteConnection> connection;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_connected) {
                return std::unexpected(Error{"disconnected", "Client is not connected"});
            }
            id = "request-" + std::to_string(++m_requestSequence);
            m_pending.emplace(id, entry);
            connection = m_connection;
        }
        auto frame = m_codec.encode(ProtocolSide::Client, Json{{"type", "request"}, {"id", id}, {"target", target}, {"call", call}}, m_maxFrameLength);
        if (!frame) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.erase(id);
            return std::unexpected(frame.error());
        }
        if (auto sent = connection->send(*frame); !sent) {
            fail(Error{"disconnected", sent.error().message});
        }
        std::uint64_t listenerId = 0;
        if (signal) {
            listenerId = signal->onAbort([this, id, target, entry] { cancel(id, target, entry); });
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [&] { return entry->done; });
        lock.unlock();
        if (signal) {
            signal->removeListener(listenerId);
        }
        if (entry->error) {
            return std::unexpected(*entry->error);
        }
        return entry->result;
    }

    /** Fails the waiting call at once and asks the server to stop working on it. */
    void cancel(const std::string& id, const Json& target, const std::shared_ptr<PendingClientRequest>& entry) {
        std::shared_ptr<IByteConnection> connection;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (entry->done) {
                return;
            }
            entry->error = Error{"aborted", "The operation was aborted"};
            entry->done = true;
            connection = m_connection;
        }
        m_changed.notify_all();
        auto frame = m_codec.encode(ProtocolSide::Client, Json{{"type", "cancel"}, {"id", id}, {"target", target}}, m_maxFrameLength);
        if (frame && connection) {
            (void)connection->send(*frame);
        }
    }

    void onData(std::string_view chunk) {
        std::vector<Json> messages;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_connecting && !m_connected) {
                return;
            }
        }
        auto decoded = m_decoder.push(chunk);
        if (!decoded) {
            fail(decoded.error());
            return;
        }
        for (const Json& message : *decoded) {
            if (!handle(message)) {
                return;
            }
        }
    }

    void onClose() {
        if (auto ended = m_decoder.end(); !ended) {
            fail(ended.error());
        } else {
            fail(Error{"disconnected", "Byte transport closed"});
        }
        // Notified under the lock: the destructor waits for this flag and may destroy the condition variable right after.
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_readerActive = false;
        m_changed.notify_all();
    }

    /** One server message; false once it disconnected the client. */
    bool handle(const Json& message) {
        const std::string type = message["type"].get<std::string>();
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_connecting) {
            return handshake(message, lock);
        }
        if (!m_connected) {
            return false;
        }
        if (type == "hello" || type == "hello_error") {
            lock.unlock();
            fail(Error{"protocol_validation", "Unexpected handshake message"});
            return false;
        }
        if (type == "attachment") {
            return handleAttachment(message, lock);
        }
        if (type == "service_update") {
            return handleUpdate(message, lock);
        }
        return handleResponse(message, lock);
    }

    bool handshake(const Json& message, std::unique_lock<std::mutex>& lock) {
        const std::string type = message["type"].get<std::string>();
        if (type == "hello_error") {
            const Json& error = message["error"];
            lock.unlock();
            fail(Error{error["code"].get<std::string>(), error["message"].get<std::string>()});
            return false;
        }
        if (type != "hello") {
            lock.unlock();
            fail(Error{"protocol_validation", "Expected server hello as first message"});
            return false;
        }
        if (message["serverId"].get<std::string>() != m_serverId) {
            const std::string actual = message["serverId"].get<std::string>();
            lock.unlock();
            fail(Error{"protocol_validation", "Connected server \"" + actual + "\" does not match \"" + m_serverId + "\""});
            return false;
        }
        m_hello = message;
        m_connected = true;
        m_connecting = false;
        lock.unlock();
        m_changed.notify_all();
        return true;
    }

    bool handleAttachment(const Json& message, std::unique_lock<std::mutex>& lock) {
        const Json attachment = message["attachment"];
        if (!attachment.is_null() && attachment["serverId"].get<std::string>() != m_serverId) {
            lock.unlock();
            fail(Error{"protocol_validation", "Attachment update belongs to another server"});
            return false;
        }
        const bool changed = attachment.is_null() ? m_attachment.has_value() : !m_attachment || *m_attachment != attachment;
        if (!changed) {
            return true;
        }
        if (attachment.is_null()) {
            m_attachment.reset();
        } else {
            m_attachment = attachment;
        }
        const std::vector<AttachmentListener> listeners = m_attachmentListeners;
        lock.unlock();
        for (const AttachmentListener& listener : listeners) {
            enqueue([listener, attachment] { listener(attachment); });
        }
        return true;
    }

    bool handleUpdate(const Json& message, std::unique_lock<std::mutex>& lock) {
        const std::string id = message["subscriptionId"].get<std::string>();
        const auto found = m_subscriptions.find(id);
        if (found == m_subscriptions.end()) {
            return true;
        }
        if (auto valid = m_wire.validateUpdate(message["update"], true); !valid) {
            lock.unlock();
            fail(Error{"protocol_validation", valid.error().message});
            return false;
        }
        ClientSubscriptionState& state = found->second;
        if (!state.hydrated) {
            state.queuedWire.push_back(message["update"]);
            return true;
        }
        auto update = m_decoders.at(id).decodeUpdate(message["update"]);
        if (!update) {
            lock.unlock();
            fail(Error{"protocol_validation", update.error().message});
            return false;
        }
        if (!state.ready) {
            state.queued.push_back(std::move(*update));
            return true;
        }
        lock.unlock();
        deliverUpdate(id, std::move(*update));
        return true;
    }

    bool handleResponse(const Json& message, std::unique_lock<std::mutex>& lock) {
        const std::string id = message["id"].get<std::string>();
        const auto found = m_pending.find(id);
        if (found == m_pending.end()) {
            lock.unlock();
            fail(Error{"protocol_validation", "Response has no matching request"});
            return false;
        }
        const std::shared_ptr<PendingClientRequest> entry = found->second;
        m_pending.erase(found);
        if (entry->done) {
            return true;
        }
        if (!message["ok"].get<bool>()) {
            entry->error = Error{message["error"]["code"].get<std::string>(), message["error"]["message"].get<std::string>()};
        } else if (entry->transform) {
            auto transformed = entry->transform(message.contains("result") ? message["result"] : Json());
            if (transformed) {
                entry->result = std::move(*transformed);
            } else {
                entry->error = Error{"protocol_validation", transformed.error().message};
                entry->done = true;
                lock.unlock();
                m_changed.notify_all();
                fail(*entry->error);
                return false;
            }
        } else if (message.contains("result")) {
            entry->result = message["result"];
        }
        entry->done = true;
        lock.unlock();
        m_changed.notify_all();
        return true;
    }

    /** Runs on the reader thread, under the lock, when a subscribe response arrives. */
    Result<Json> hydrate(const std::string& id, const Json& wire) {
        if (auto valid = m_wire.validateSubscriptionSnapshot(wire, true); !valid) {
            return std::unexpected(valid.error());
        }
        const auto found = m_subscriptions.find(id);
        if (found == m_subscriptions.end()) {
            return std::unexpected(Error{"disconnected", "The subscription ended"});
        }
        ServiceStateDecoder& decoder = m_decoders.at(id);
        auto snapshot = decoder.decodeSnapshot(wire);
        if (!snapshot) {
            return snapshot;
        }
        found->second.hydrated = true;
        for (const Json& pending : found->second.queuedWire) {
            auto update = decoder.decodeUpdate(pending);
            if (!update) {
                return std::unexpected(update.error());
            }
            found->second.queued.push_back(std::move(*update));
        }
        found->second.queuedWire.clear();
        return snapshot;
    }

    void deliverUpdate(const std::string& id, Json update) {
        enqueue([this, id, update = std::move(update)] {
            UpdateListener listener;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                const auto found = m_subscriptions.find(id);
                if (found == m_subscriptions.end()) {
                    return;
                }
                listener = found->second.listener;
            }
            if (listener) {
                listener(update);
            }
        });
    }

    /** Ends the connection with `error` unless it already ended: fails what waits and closes the transport. */
    void fail(const Error& error) {
        std::shared_ptr<IByteConnection> connection;
        DisconnectListener listener;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_connecting && !m_connected) {
                return;
            }
            const bool wasConnected = m_connected;
            m_connecting = false;
            m_connected = false;
            m_failure = error;
            connection = std::move(m_connection);
            m_connection.reset();
            for (auto& entry : m_pending) {
                if (!entry.second->done) {
                    entry.second->error = error;
                    entry.second->done = true;
                }
            }
            m_pending.clear();
            m_subscriptions.clear();
            m_decoders.clear();
            const bool hadAttachment = m_attachment.has_value();
            m_attachment.reset();
            m_hello.reset();
            if (wasConnected) {
                listener = m_disconnectListener;
            }
            if (hadAttachment) {
                for (const AttachmentListener& attachmentListener : m_attachmentListeners) {
                    enqueue([attachmentListener] { attachmentListener(Json()); });
                }
            }
        }
        m_changed.notify_all();
        if (connection) {
            connection->close();
        }
        if (listener) {
            enqueue([listener, error] { listener(error); });
        }
    }

    void enqueue(std::function<void()> work) {
        {
            const std::lock_guard<std::mutex> lock(m_deliveryMutex);
            m_deliveryQueue.push_back(std::move(work));
        }
        m_deliveryWake.notify_all();
    }

    void deliveryLoop() {
        while (true) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(m_deliveryMutex);
                m_deliveryWake.wait(lock, [&] { return m_stopping || !m_deliveryQueue.empty(); });
                if (m_deliveryQueue.empty()) {
                    return;
                }
                work = std::move(m_deliveryQueue.front());
                m_deliveryQueue.pop_front();
            }
            work();
        }
    }

    IClientTransport& m_transport;
    std::string m_serverId;
    std::uint64_t m_maxFrameLength;
    std::int64_t m_handshakeTimeoutMs;
    ProtocolCodec m_codec;
    ServiceWire m_wire;
    ProtocolMessageDecoder m_decoder;

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::shared_ptr<IByteConnection> m_connection;
    bool m_connecting = false;
    bool m_connected = false;
    bool m_readerActive = false;
    std::optional<Error> m_failure;
    std::optional<Json> m_hello;
    std::optional<Json> m_attachment;
    std::uint64_t m_requestSequence = 0;
    std::uint64_t m_subscriptionSequence = 0;
    std::map<std::string, std::shared_ptr<PendingClientRequest>> m_pending;
    std::map<std::string, ClientSubscriptionState> m_subscriptions;
    std::map<std::string, ServiceStateDecoder> m_decoders;
    std::vector<AttachmentListener> m_attachmentListeners;
    DisconnectListener m_disconnectListener;

    std::mutex m_deliveryMutex;
    std::condition_variable m_deliveryWake;
    std::deque<std::function<void()>> m_deliveryQueue;
    bool m_stopping = false;
    std::thread m_delivery;
};
