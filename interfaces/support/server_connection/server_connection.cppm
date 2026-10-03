module;

#include <cstdint>

export module pi.support.server_connection;

import std;
export import pi.server.i_byte_connection;
export import pi.server.i_byte_connection_handler;
export import pi.server.i_server_presentation;
export import pi.support.protocol_message_decoder;
export import pi.support.service_state_encoder;
export import pi.support.service_wire;
export import pi.types.active_request;
export import pi.types.connection_stage;
export import pi.types.pending_subscription;
export import pi.types.server_connection_options;
export import pi.types.session_attachment;

/**
 * One client connection of a protocol server: decodes its messages, runs the hello handshake,
 * dispatches requests (server-wide services or the attached session) to the executor, cancels them,
 * encodes service updates per subscription and tears everything down when the connection ends.
 * Messages to the client are encoded and sent under one lock so their order is their encoding
 * order. Port of the connection handling in packages/server/src/server.ts.
 */
export class ServerConnection : public IByteConnectionHandler,
                                public IServerPresentation,
                                public std::enable_shared_from_this<ServerConnection> {
public:
    ServerConnection(std::shared_ptr<IByteConnection> connection, ServerConnectionOptions options);

    void onData(std::string_view chunk) override;
    void onClose() override;
    void onError(const Error& error) override;

    Result<void> attachSession(const std::string& sessionId, const ServiceContext& context) override;
    Result<void> detachSession(const ServiceContext& context) override;
    Result<void> prepareSessionRemoval(const std::string& sessionId, const ServiceContext& context) override;

    std::uint64_t clientId() const;
    ConnectionStage stage();
    /** Tells the client which session it is attached to, or none. */
    void sendAttachment(const std::optional<SessionAttachment>& attachment);
    /** Fails the connection when the hello has not completed in time. */
    void checkHandshakeTimeout();
    /** The server is closing: ends the connection now. */
    void shutdown();

private:
    void dispatch(const Json& message);
    void finishHandshake(const Json& hello);
    void handleCancel(const Json& envelope);
    void beginRequest(const Json& envelope);
    void runRequest(const Json& envelope, const std::shared_ptr<ActiveRequest>& active);
    Result<std::optional<Json>> dispatchCall(const Json& envelope, const ActiveRequest& active,
                                             const std::optional<ServiceControlCall>& control,
                                             const IServiceEndpoint::Publisher& publish, const ServiceContext& context);
    Result<std::optional<Json>> installSubscription(const std::string& subscriptionId, const Json& snapshot);
    void drainPending(const std::shared_ptr<PendingSubscription>& pending);
    void sendServiceUpdate(const std::string& subscriptionId, const Json& update);
    void respond(const Json& id, const Result<std::optional<Json>>& result, bool cancelled);
    bool sendMessage(const Json& message);
    bool transmit(const Json& message);
    void failProtocol(const Error& error);
    void disconnect();
    void drop(const Error& reason);
    RpcTarget parseTarget(const Json& target) const;
    bool sameTarget(const RpcTarget& left, const RpcTarget& right) const;
    Error protocolError(const Error& error) const;
    bool terminal();
    bool closing() const;
    ServiceContext background() const;
    void report(const Error& error) const;

    std::shared_ptr<IByteConnection> m_connection;
    ServerConnectionOptions m_options;
    ProtocolMessageDecoder m_decoder;
    ProtocolCodec m_codec;
    ServiceWire m_wire;
    std::chrono::steady_clock::time_point m_handshakeDeadline;

    /** Guards the fields below. */
    std::mutex m_mutex;
    ConnectionStage m_stage = ConnectionStage::AwaitingHello;
    bool m_disconnected = false;
    std::map<std::string, std::shared_ptr<ActiveRequest>> m_active;
    std::map<std::string, std::shared_ptr<ServiceStateEncoder>> m_encoders;
    std::shared_ptr<IServiceAttachment> m_services;

    /** Serialises encoding and sending of messages to the client. */
    std::mutex m_sendMutex;
};

ServerConnection::ServerConnection(std::shared_ptr<IByteConnection> connection, ServerConnectionOptions options)
    : m_connection(std::move(connection)),
      m_options(std::move(options)),
      m_decoder(ProtocolSide::Client, m_options.maxFrameLength),
      m_handshakeDeadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(m_options.handshakeTimeoutMs)) {}

std::uint64_t ServerConnection::clientId() const {
    return m_options.clientId;
}

bool ServerConnection::closing() const {
    return m_options.isClosing && m_options.isClosing();
}

ServiceContext ServerConnection::background() const {
    return ServiceContext{std::make_shared<AbortSignal>()};
}

void ServerConnection::report(const Error& error) const {
    if (m_options.reportError) {
        m_options.reportError(error);
    }
}

ConnectionStage ServerConnection::stage() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_stage;
}

bool ServerConnection::terminal() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_disconnected || m_stage == ConnectionStage::Closing || m_stage == ConnectionStage::Closed;
}

Result<void> ServerConnection::attachSession(const std::string& sessionId, const ServiceContext& context) {
    return m_options.router->attachClient(m_options.clientId, sessionId, context);
}

Result<void> ServerConnection::detachSession(const ServiceContext& context) {
    return m_options.router->detachClient(m_options.clientId, context);
}

Result<void> ServerConnection::prepareSessionRemoval(const std::string& sessionId, const ServiceContext& context) {
    return m_options.router->removeSession(sessionId, context);
}

Error ServerConnection::protocolError(const Error& error) const {
    static const std::set<std::string> known = {
        "service_not_allowed", "service_not_found", "service_mode_mismatch", "service_member_not_found",
        "service_member_mismatch", "service_instance_not_found", "service_stale_instance", "service_invalid_value",
        "wrong_server", "session_not_found", "session_ambiguous", "session_not_attached", "server_draining",
        "invalid_request"};
    if (known.contains(error.code)) {
        return error;
    }
    if (error.code == "protocol_validation") {
        return Error{"invalid_request", error.message};
    }
    report(error);
    return Error{"internal_error", "Internal server error"};
}

RpcTarget ServerConnection::parseTarget(const Json& target) const {
    RpcTarget result;
    result.serverId = target["serverId"].get<std::string>();
    if (target.contains("sessionId")) {
        result.sessionId = target["sessionId"].get<std::string>();
        result.attachmentId = target["attachmentId"].get<std::string>();
    }
    return result;
}

bool ServerConnection::sameTarget(const RpcTarget& left, const RpcTarget& right) const {
    return left.serverId == right.serverId && left.sessionId == right.sessionId &&
           left.attachmentId == right.attachmentId;
}

void ServerConnection::onData(std::string_view chunk) {
    if (terminal()) {
        return;
    }
    auto messages = m_decoder.push(chunk);
    if (!messages) {
        failProtocol(protocolError(messages.error()));
        return;
    }
    for (const Json& message : *messages) {
        if (terminal()) {
            return;
        }
        dispatch(message);
    }
}

void ServerConnection::onClose() {
    bool ending = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ending = !m_disconnected && m_stage != ConnectionStage::Closing;
    }
    if (ending) {
        if (auto ended = m_decoder.end(); !ended) {
            report(ended.error());
        }
    }
    disconnect();
}

void ServerConnection::onError(const Error& error) {
    report(error);
    drop(error);
}

void ServerConnection::drop(const Error&) {
    m_connection->close();
    disconnect();
}

void ServerConnection::shutdown() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stage = ConnectionStage::Closing;
    }
    m_connection->close();
    disconnect();
}

void ServerConnection::checkHandshakeTimeout() {
    if (std::chrono::steady_clock::now() < m_handshakeDeadline) {
        return;
    }
    const ConnectionStage current = stage();
    if (current == ConnectionStage::AwaitingHello || current == ConnectionStage::Handshaking) {
        failProtocol(Error{"invalid_request", "Handshake timeout"});
    }
}

void ServerConnection::dispatch(const Json& message) {
    const std::string type = message["type"].get<std::string>();
    ConnectionStage current = stage();
    if (current == ConnectionStage::AwaitingHello) {
        if (type != "hello") {
            failProtocol(Error{"invalid_request", "The first client message must be hello"});
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_stage = ConnectionStage::Handshaking;
        }
        finishHandshake(message);
        return;
    }
    if (type == "hello") {
        failProtocol(Error{"invalid_request", "hello may only be sent as the first message"});
        return;
    }
    if (current != ConnectionStage::Ready) {
        return;
    }
    if (type == "cancel") {
        handleCancel(message);
    } else {
        beginRequest(message);
    }
}

void ServerConnection::finishHandshake(const Json& hello) {
    const int version = static_cast<int>(hello["version"].get<double>());
    if (version != ProtocolValidator::kProtocolVersion) {
        failProtocol(Error{"version", "Unsupported protocol version " + std::to_string(version) + "; expected " +
                                          std::to_string(ProtocolValidator::kProtocolVersion)});
        return;
    }
    if (closing() || terminal() || m_connection->closed()) {
        return;
    }
    auto attached = m_options.host->serverServices().attachClient(*this, background());
    if (!attached) {
        failProtocol(protocolError(attached.error()));
        return;
    }
    std::shared_ptr<IServiceAttachment> services = std::move(*attached);
    if (closing() || terminal() || m_connection->closed()) {
        services->release(background());
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_services = services;
    }
    const Json reply = {{"type", "hello"}, {"version", ProtocolValidator::kProtocolVersion}, {"serverId", m_options.serverId}};
    if (!sendMessage(reply)) {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stage == ConnectionStage::Handshaking) {
        m_stage = ConnectionStage::Ready;
    }
}

void ServerConnection::handleCancel(const Json& envelope) {
    const RpcTarget target = parseTarget(envelope["target"]);
    if (target.serverId != m_options.serverId) {
        return;
    }
    std::shared_ptr<AbortSignal> signal;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_active.find(envelope["id"].get<std::string>());
        if (found != m_active.end() && sameTarget(found->second->target, target)) {
            signal = found->second->signal;
        }
    }
    if (signal) {
        signal->abort();
    }
}

void ServerConnection::beginRequest(const Json& envelope) {
    const std::string id = envelope["id"].get<std::string>();
    bool duplicate = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        duplicate = m_active.contains(id);
    }
    if (duplicate) {
        respond(id, std::unexpected(Error{"invalid_request", "Request ID is already active"}), false);
        return;
    }
    if (!m_wire.validateCall(envelope["call"])) {
        respond(id, std::unexpected(Error{"invalid_request", "Invalid service call"}), false);
        return;
    }
    auto active = std::make_shared<ActiveRequest>();
    active->signal = std::make_shared<AbortSignal>();
    active->target = parseTarget(envelope["target"]);
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_active[id] = active;
    }
    const std::shared_ptr<ServerConnection> self = shared_from_this();
    m_options.executor->submit([self, envelope, active] { self->runRequest(envelope, active); });
}

void ServerConnection::runRequest(const Json& envelope, const std::shared_ptr<ActiveRequest>& active) {
    const std::string id = envelope["id"].get<std::string>();
    const std::optional<ServiceControlCall> control = m_wire.decodeControl(envelope["call"]);
    const bool subscribing = control && control->type == "subscribe";
    auto pending = std::make_shared<PendingSubscription>();
    if (subscribing) {
        pending->subscriptionId = control->subscriptionId;
        pending->ready = false;
    }
    const std::shared_ptr<ServerConnection> self = shared_from_this();
    const IServiceEndpoint::Publisher publish = [self, pending](const std::string& subscriptionId, const Json& update,
                                                                const ServiceContext&) {
        if (subscriptionId == pending->subscriptionId) {
            const std::lock_guard<std::mutex> lock(pending->mutex);
            if (!pending->ready) {
                pending->updates.push_back(update);
                return;
            }
        }
        self->sendServiceUpdate(subscriptionId, update);
    };
    const ServiceContext context{active->signal};
    auto result = dispatchCall(envelope, *active, control, publish, context);
    if (result && subscribing) {
        result = installSubscription(control->subscriptionId, result->value_or(Json()));
    } else if (result && control && control->type == "unsubscribe") {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_encoders.erase(control->subscriptionId);
    }
    respond(id, result, active->signal->aborted());
    if (subscribing && result) {
        drainPending(pending);
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_active.find(id);
    if (found != m_active.end() && found->second == active) {
        m_active.erase(found);
    }
}

Result<std::optional<Json>> ServerConnection::dispatchCall(const Json& envelope, const ActiveRequest& active,
                                                           const std::optional<ServiceControlCall>& control,
                                                           const IServiceEndpoint::Publisher& publish,
                                                           const ServiceContext& context) {
    if (active.target.serverId != m_options.serverId) {
        return std::unexpected(Error{"wrong_server", "Request was addressed to another server"});
    }
    const Json& call = envelope["call"];
    std::shared_ptr<IServiceAttachment> services;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (control && control->type == "subscribe" && m_encoders.contains(control->subscriptionId)) {
            return std::unexpected(
                Error{"invalid_request", "Duplicate service subscription " + control->subscriptionId});
        }
        services = m_services;
    }
    if (active.target.sessionId) {
        return m_options.router->executeServiceCall(call, active.target, m_options.clientId, publish, context);
    }
    if (!services) {
        return std::unexpected(Error{"invalid_request", "Unknown service member " + call["serviceId"].get<std::string>() +
                                                            "." + call["member"].get<std::string>()});
    }
    return services->invokeService(call, publish, context);
}

Result<std::optional<Json>> ServerConnection::installSubscription(const std::string& subscriptionId,
                                                                  const Json& snapshot) {
    if (snapshot.is_null()) {
        return std::unexpected(Error{"invalid_request", "Service subscription did not return a snapshot"});
    }
    if (auto valid = m_wire.validateSubscriptionSnapshot(snapshot, false); !valid) {
        return std::unexpected(valid.error());
    }
    const std::lock_guard<std::mutex> sending(m_sendMutex);
    auto encoder = std::make_shared<ServiceStateEncoder>();
    auto encoded = encoder->encodeSnapshot(snapshot);
    if (!encoded) {
        return std::unexpected(Error{"invalid_request", encoded.error().message});
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_encoders[subscriptionId] = encoder;
    return std::optional<Json>(std::move(*encoded));
}

void ServerConnection::drainPending(const std::shared_ptr<PendingSubscription>& pending) {
    while (true) {
        Json update;
        {
            const std::lock_guard<std::mutex> lock(pending->mutex);
            if (pending->updates.empty()) {
                pending->ready = true;
                return;
            }
            update = std::move(pending->updates.front());
            pending->updates.erase(pending->updates.begin());
        }
        sendServiceUpdate(pending->subscriptionId, update);
    }
}

void ServerConnection::sendServiceUpdate(const std::string& subscriptionId, const Json& update) {
    const std::lock_guard<std::mutex> sending(m_sendMutex);
    std::shared_ptr<ServiceStateEncoder> encoder;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_encoders.find(subscriptionId);
        if (found == m_encoders.end()) {
            return;
        }
        encoder = found->second;
    }
    auto encoded = encoder->encodeUpdate(update);
    if (!encoded) {
        report(encoded.error());
        m_connection->close();
        disconnect();
        return;
    }
    transmit(Json{{"type", "service_update"}, {"subscriptionId", subscriptionId}, {"update", std::move(*encoded)}});
}

void ServerConnection::respond(const Json& id, const Result<std::optional<Json>>& result, bool cancelled) {
    Json message = {{"type", "response"}, {"id", id}};
    if (result) {
        message["ok"] = true;
        if (result->has_value()) {
            message["result"] = **result;
        }
    } else {
        const Error error = cancelled ? Error{"cancelled", "RPC request cancelled"} : protocolError(result.error());
        message["ok"] = false;
        message["error"] = Json{{"code", error.code}, {"message", error.message}};
    }
    sendMessage(message);
}

void ServerConnection::sendAttachment(const std::optional<SessionAttachment>& attachment) {
    Json value;
    if (attachment) {
        value = Json{{"serverId", m_options.serverId},
                     {"sessionId", attachment->sessionId},
                     {"attachmentId", attachment->attachmentId}};
    }
    sendMessage(Json{{"type", "attachment"}, {"attachment", value}});
}

bool ServerConnection::sendMessage(const Json& message) {
    const std::lock_guard<std::mutex> sending(m_sendMutex);
    return transmit(message);
}

bool ServerConnection::transmit(const Json& message) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disconnected) {
            return false;
        }
    }
    if (m_connection->closed()) {
        return false;
    }
    auto frame = m_codec.encode(ProtocolSide::Server, message, m_options.maxFrameLength);
    if (!frame) {
        report(frame.error());
        m_connection->close();
        disconnect();
        return false;
    }
    if (auto sent = m_connection->send(*frame); !sent) {
        report(sent.error());
        m_connection->close();
        disconnect();
        return false;
    }
    return true;
}

void ServerConnection::failProtocol(const Error& error) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disconnected || m_stage == ConnectionStage::Closing || m_stage == ConnectionStage::Closed) {
            return;
        }
        m_stage = ConnectionStage::Closing;
    }
    const Json message = {{"type", "hello_error"}, {"error", Json{{"code", error.code}, {"message", error.message}}}};
    auto frame = m_codec.encode(ProtocolSide::Server, message, m_options.maxFrameLength);
    if (!frame) {
        report(frame.error());
    }
    m_connection->close(frame ? std::string_view(*frame) : std::string_view());
    disconnect();
}

void ServerConnection::disconnect() {
    std::vector<std::shared_ptr<AbortSignal>> signals;
    std::shared_ptr<IServiceAttachment> services;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_disconnected) {
            return;
        }
        m_disconnected = true;
        m_stage = ConnectionStage::Closed;
        for (const auto& entry : m_active) {
            signals.push_back(entry.second->signal);
        }
        m_active.clear();
        m_encoders.clear();
        services = std::move(m_services);
    }
    for (const auto& signal : signals) {
        signal->abort();
    }
    if (m_options.onDisconnect) {
        m_options.onDisconnect(m_options.clientId);
    }
    const std::shared_ptr<ServerConnection> self = shared_from_this();
    m_options.executor->submit([self, services] {
        self->m_options.router->disconnect(self->m_options.clientId, self->background());
        if (services) {
            services->release(self->background());
        }
    });
}
