module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.mcp.mcp_client;

import std;
export import pi.mcp.i_mcp_client;
export import pi.mcp.i_mcp_transport;
export import pi.support.mcp_message_codec;
export import pi.support.mcp_result_parser;
export import pi.types.mcp_client_options;
export import pi.types.mcp_client_state;
export import pi.types.mcp_pending_request;

/**
 * MCP client over any IMcpTransport. Requests block the calling thread until the response, the
 * timeout (reset by progress notifications) or an abort; responses and progress are handled on the
 * transport's reader thread, everything else (notification listeners, server requests such as ping
 * and roots/list) on a dispatcher thread so that listeners can issue requests themselves.
 * Port of packages/mcp/src/client.ts.
 */
export class McpClient : public IMcpClient {
public:
    explicit McpClient(McpClientOptions options);
    ~McpClient() override;

    /** Takes over the transport, starts it and runs the initialize handshake; returns the result. */
    Result<Json> connect(std::unique_ptr<IMcpTransport> transport);

    Result<Json> request(const std::string& method, const Json& params, const McpRequestOptions& options) override;
    Result<void> notify(const std::string& method, const Json& params) override;
    Result<std::vector<McpTool>> listTools(const McpRequestOptions& options) override;
    Result<McpCallResult> callTool(const std::string& name, const Json& arguments,
                                   const McpRequestOptions& options) override;
    Result<std::vector<Json>> listResources(const McpRequestOptions& options) override;
    Result<std::vector<Json>> listResourceTemplates(const McpRequestOptions& options) override;
    Result<Json> readResource(const std::string& uri, const McpRequestOptions& options) override;
    Json serverCapabilities() const override;
    std::optional<std::string> instructions() const override;
    bool connected() const override;
    void onNotification(const std::string& method, std::function<void(const Json&)> listener) override;
    void onClose(std::function<void()> listener) override;
    void close() override;

    Json serverInfo() const;
    std::optional<std::string> protocolVersion() const;
    McpClientState state() const;

private:
    static constexpr int MaxListPages = 1000;

    Result<Json> requestInternal(const std::string& method, const Json& params, const McpRequestOptions& options,
                                 bool allowConnecting);
    Result<void> notifyInternal(const std::string& method, const Json& params, bool allowConnecting);
    IMcpTransport* usableTransport(bool allowConnecting) const;
    Json withProgressToken(const Json& params, std::int64_t token) const;
    Result<Json> await(std::unique_lock<std::mutex>& lock, const std::shared_ptr<McpPendingRequest>& entry,
                       std::optional<std::string>& cancelReason);
    void removePending(std::int64_t id);
    void sendCancellation(std::int64_t id, const std::string& reason);
    void handleMessage(const Json& message);
    void handleResponse(const Json& message);
    void handleProgress(const Json& params);
    void handleServerRequest(const Json& message);
    void handleNotification(const std::string& method, const Json& params);
    void handleTransportClose();
    void markClosed(const Error& reason);
    void dispatch(std::function<void()> task);
    void dispatcherLoop();
    Result<std::vector<Json>> listAll(const std::string& method, const std::string& key, const std::string& kind,
                                      const McpRequestOptions& options);
    Result<Json> handshake();

    McpClientOptions m_options;
    McpMessageCodec m_codec;
    McpResultParser m_parser;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    McpClientState m_state = McpClientState::Idle;
    std::unique_ptr<IMcpTransport> m_transport;
    std::int64_t m_nextId = 1;
    std::map<std::int64_t, std::shared_ptr<McpPendingRequest>> m_pending;
    Json m_capabilities;
    Json m_serverInfo;
    std::optional<std::string> m_instructions;
    std::optional<std::string> m_protocolVersion;
    std::map<std::string, std::vector<std::function<void(const Json&)>>> m_listeners;
    std::vector<std::function<void()>> m_closeListeners;
    std::mutex m_queueMutex;
    std::condition_variable m_queueChanged;
    std::deque<std::function<void()>> m_queue;
    bool m_stopDispatcher = false;
    std::thread m_dispatcher;
};

McpClient::McpClient(McpClientOptions options) : m_options(std::move(options)) {}

McpClient::~McpClient() {
    close();
    {
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopDispatcher = true;
    }
    m_queueChanged.notify_all();
    if (m_dispatcher.joinable()) {
        if (m_dispatcher.get_id() == std::this_thread::get_id()) {
            m_dispatcher.detach();
        } else {
            m_dispatcher.join();
        }
    }
}

void McpClient::dispatch(std::function<void()> task) {
    {
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_stopDispatcher) {
            return;
        }
        m_queue.push_back(std::move(task));
    }
    m_queueChanged.notify_one();
}

void McpClient::dispatcherLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueChanged.wait(lock, [this] { return m_stopDispatcher || !m_queue.empty(); });
            if (m_queue.empty()) {
                return;
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

Json McpClient::serverCapabilities() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_capabilities;
}

Json McpClient::serverInfo() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_serverInfo;
}

std::optional<std::string> McpClient::instructions() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_instructions;
}

std::optional<std::string> McpClient::protocolVersion() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_protocolVersion;
}

McpClientState McpClient::state() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

bool McpClient::connected() const {
    return state() == McpClientState::Connected;
}

void McpClient::onNotification(const std::string& method, std::function<void(const Json&)> listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners[method].push_back(std::move(listener));
}

void McpClient::onClose(std::function<void()> listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_closeListeners.push_back(std::move(listener));
}

IMcpTransport* McpClient::usableTransport(bool allowConnecting) const {
    if (m_transport && (m_state == McpClientState::Connected || (allowConnecting && m_state == McpClientState::Connecting))) {
        return m_transport.get();
    }
    return nullptr;
}

Json McpClient::withProgressToken(const Json& params, std::int64_t token) const {
    Json out = params.is_object() ? params : Json::object();
    Json meta = out.contains("_meta") && out["_meta"].is_object() ? out["_meta"] : Json::object();
    meta["progressToken"] = token;
    out["_meta"] = std::move(meta);
    return out;
}

Result<Json> McpClient::await(std::unique_lock<std::mutex>& lock, const std::shared_ptr<McpPendingRequest>& entry,
                              std::optional<std::string>& cancelReason) {
    while (!entry->outcome) {
        if (entry->aborted) {
            entry->outcome = Result<Json>(std::unexpected(Error{"aborted", "MCP request aborted"}));
            cancelReason = "Aborted";
            break;
        }
        if (entry->timeoutMs > 0) {
            m_changed.wait_until(lock, entry->deadline);
            if (!entry->outcome && !entry->aborted && std::chrono::steady_clock::now() >= entry->deadline) {
                entry->outcome = Result<Json>(std::unexpected(
                    Error{"timeout", "MCP request timed out after " + std::to_string(entry->timeoutMs) + "ms"}));
                cancelReason = "Request timed out";
            }
        } else {
            m_changed.wait(lock);
        }
    }
    return *entry->outcome;
}

void McpClient::removePending(std::int64_t id) {
    m_pending.erase(id);
}

void McpClient::sendCancellation(std::int64_t id, const std::string& reason) {
    IMcpTransport* transport = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        transport = m_transport.get();
    }
    if (transport != nullptr) {
        transport->send(m_codec.notification("notifications/cancelled", Json{{"requestId", id}, {"reason", reason}}));
    }
}

Result<Json> McpClient::requestInternal(const std::string& method, const Json& params, const McpRequestOptions& options,
                                        bool allowConnecting) {
    if (options.signal && options.signal->aborted()) {
        return std::unexpected(Error{"aborted", "MCP request aborted"});
    }
    auto entry = std::make_shared<McpPendingRequest>();
    entry->method = method;
    entry->cancellable = method != "initialize";
    entry->onProgress = options.onProgress;
    entry->timeoutMs = options.timeoutMs.value_or(m_options.requestTimeoutMs);
    IMcpTransport* transport = nullptr;
    Json message;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        transport = usableTransport(allowConnecting);
        if (transport == nullptr) {
            return std::unexpected(Error{"closed", "MCP client is not connected"});
        }
        entry->id = m_nextId++;
        const Json requestParams = options.onProgress ? withProgressToken(params, entry->id) : params;
        message = m_codec.request(entry->id, method, requestParams);
        if (entry->timeoutMs > 0) {
            entry->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(entry->timeoutMs);
        }
        m_pending[entry->id] = entry;
    }
    std::uint64_t abortListener = 0;
    if (options.signal) {
        abortListener = options.signal->onAbort([this, entry]() {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                entry->aborted = true;
            }
            m_changed.notify_all();
        });
    }
    if (auto sent = transport->send(message); !sent) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        removePending(entry->id);
        if (options.signal) {
            options.signal->removeListener(abortListener);
        }
        return std::unexpected(sent.error());
    }
    std::optional<std::string> cancelReason;
    Result<Json> outcome = Json();
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        outcome = await(lock, entry, cancelReason);
        removePending(entry->id);
    }
    if (options.signal) {
        options.signal->removeListener(abortListener);
    }
    if (cancelReason && entry->cancellable) {
        sendCancellation(entry->id, *cancelReason);
    }
    return outcome;
}

Result<void> McpClient::notifyInternal(const std::string& method, const Json& params, bool allowConnecting) {
    IMcpTransport* transport = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        transport = usableTransport(allowConnecting);
    }
    if (transport == nullptr) {
        return std::unexpected(Error{"closed", "MCP client is not connected"});
    }
    return transport->send(m_codec.notification(method, params));
}

Result<Json> McpClient::request(const std::string& method, const Json& params, const McpRequestOptions& options) {
    return requestInternal(method, params, options, false);
}

Result<void> McpClient::notify(const std::string& method, const Json& params) {
    return notifyInternal(method, params, false);
}

Result<Json> McpClient::handshake() {
    Json capabilities = Json::object();
    if (m_options.roots.is_array()) {
        capabilities["roots"] = Json::object();
    }
    Json clientInfo = Json{{"name", m_options.name}, {"version", m_options.version}};
    if (m_options.title) {
        clientInfo["title"] = *m_options.title;
    }
    auto raw = requestInternal("initialize",
                               Json{{"protocolVersion", "2025-11-25"},
                                    {"capabilities", std::move(capabilities)},
                                    {"clientInfo", std::move(clientInfo)}},
                               McpRequestOptions{}, true);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    auto parsed = m_parser.initialize(*raw);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const std::string version = (*parsed)["protocolVersion"].get<std::string>();
    const std::set<std::string> supported{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    if (!supported.contains(version)) {
        return std::unexpected(Error{"protocol", "MCP server selected unsupported protocol version " + version});
    }
    IMcpTransport* transport = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_protocolVersion = version;
        m_serverInfo = (*parsed)["serverInfo"];
        m_capabilities = (*parsed)["capabilities"];
        if (parsed->contains("instructions")) {
            m_instructions = (*parsed)["instructions"].get<std::string>();
        }
        transport = m_transport.get();
    }
    transport->setProtocolVersion(version);
    if (auto initialized = notifyInternal("notifications/initialized", Json(), true); !initialized) {
        return std::unexpected(initialized.error());
    }
    return *parsed;
}

Result<Json> McpClient::connect(std::unique_ptr<IMcpTransport> transport) {
    IMcpTransport* raw = transport.get();
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != McpClientState::Idle) {
            return std::unexpected(Error{"closed", "Cannot connect an MCP client that was used before"});
        }
        m_state = McpClientState::Connecting;
        m_transport = std::move(transport);
    }
    m_dispatcher = std::thread([this]() { dispatcherLoop(); });
    raw->setMessageListener([this](const Json& message) { handleMessage(message); });
    raw->setErrorListener([](const Error&) {});
    raw->setCloseListener([this]() { handleTransportClose(); });
    if (auto started = raw->start(); !started) {
        close();
        return std::unexpected(started.error());
    }
    auto result = handshake();
    if (!result) {
        close();
        return std::unexpected(result.error());
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state == McpClientState::Connecting) {
            m_state = McpClientState::Connected;
        }
    }
    return result;
}

void McpClient::handleMessage(const Json& message) {
    if (m_codec.isResponse(message)) {
        handleResponse(message);
    } else if (m_codec.isRequest(message)) {
        dispatch([this, message]() { handleServerRequest(message); });
    } else if (m_codec.isNotification(message)) {
        const std::string method = message["method"].get<std::string>();
        const Json params = message.contains("params") ? message["params"] : Json();
        if (method == "notifications/progress") {
            handleProgress(params);
        }
        handleNotification(method, params);
    }
}

void McpClient::handleResponse(const Json& message) {
    if (!message["id"].is_number_integer()) {
        return;
    }
    const std::int64_t id = message["id"].get<std::int64_t>();
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_pending.find(id);
        if (found == m_pending.end()) {
            return;
        }
        if (message.contains("error")) {
            const Json& error = message["error"];
            found->second->outcome = Result<Json>(std::unexpected(
                Error{m_codec.rpcCode(error["code"].get<int>()), error["message"].get<std::string>()}));
        } else {
            found->second->outcome = Result<Json>(message["result"]);
        }
    }
    m_changed.notify_all();
}

void McpClient::handleProgress(const Json& params) {
    if (!params.is_object() || !params.contains("progressToken") || !params["progressToken"].is_number_integer() ||
        !params.contains("progress") || !params["progress"].is_number()) {
        return;
    }
    std::function<void(const McpProgress&)> callback;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_pending.find(params["progressToken"].get<std::int64_t>());
        if (found == m_pending.end()) {
            return;
        }
        if (found->second->timeoutMs > 0) {
            found->second->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(found->second->timeoutMs);
        }
        callback = found->second->onProgress;
    }
    m_changed.notify_all();
    if (callback) {
        McpProgress progress;
        progress.progress = params["progress"].get<double>();
        if (params.contains("total") && params["total"].is_number()) {
            progress.total = params["total"].get<double>();
        }
        if (params.contains("message") && params["message"].is_string()) {
            progress.message = params["message"].get<std::string>();
        }
        callback(progress);
    }
}

void McpClient::handleNotification(const std::string& method, const Json& params) {
    std::vector<std::function<void(const Json&)>> listeners;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_listeners.find(method);
        if (found != m_listeners.end()) {
            listeners = found->second;
        }
    }
    for (auto& listener : listeners) {
        dispatch([listener, params]() { listener(params); });
    }
}

void McpClient::handleServerRequest(const Json& message) {
    const std::string method = message["method"].get<std::string>();
    Json reply;
    if (method == "ping") {
        reply = m_codec.result(message["id"], Json::object());
    } else if (method == "roots/list" && m_options.roots.is_array()) {
        reply = m_codec.result(message["id"], Json{{"roots", m_options.roots}});
    } else {
        reply = m_codec.error(message["id"], McpMessageCodec::MethodNotFound, "Method not found: " + method);
    }
    IMcpTransport* transport = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        transport = m_transport.get();
    }
    if (transport != nullptr) {
        transport->send(reply);
    }
}

void McpClient::handleTransportClose() {
    markClosed(Error{"closed", "MCP connection closed"});
}

void McpClient::markClosed(const Error& reason) {
    std::vector<std::function<void()>> listeners;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const bool wasClosed = m_state == McpClientState::Closed;
        m_state = McpClientState::Closed;
        for (auto& entry : m_pending) {
            if (!entry.second->outcome) {
                entry.second->outcome = Result<Json>(std::unexpected(reason));
            }
        }
        if (!wasClosed) {
            listeners = std::move(m_closeListeners);
            m_closeListeners.clear();
        }
    }
    m_changed.notify_all();
    for (auto& listener : listeners) {
        dispatch(std::move(listener));
    }
}

void McpClient::close() {
    IMcpTransport* transport = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        transport = m_transport.get();
    }
    markClosed(Error{"closed", "MCP connection closed"});
    if (transport != nullptr) {
        transport->setMessageListener({});
        transport->setCloseListener({});
        transport->close();
    }
}

Result<std::vector<Json>> McpClient::listAll(const std::string& method, const std::string& key, const std::string& kind,
                                             const McpRequestOptions& options) {
    std::vector<Json> items;
    std::set<std::string> cursors;
    std::optional<std::string> cursor;
    for (int page = 0; page < MaxListPages; ++page) {
        auto raw = request(method, cursor ? Json{{"cursor", *cursor}} : Json(), options);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        std::optional<std::string> next;
        auto parsed = m_parser.listPage(method, key, kind, *raw, next);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        items.insert(items.end(), parsed->begin(), parsed->end());
        if (!next) {
            return items;
        }
        if (!cursors.insert(*next).second) {
            return std::unexpected(Error{"protocol", "MCP " + method + " returned duplicate cursor: " + *next});
        }
        cursor = next;
    }
    return std::unexpected(Error{"protocol", "MCP " + method + " exceeded " + std::to_string(MaxListPages) + " pages"});
}

Result<std::vector<McpTool>> McpClient::listTools(const McpRequestOptions& options) {
    auto items = listAll("tools/list", "tools", "tool", options);
    if (!items) {
        return std::unexpected(items.error());
    }
    std::vector<McpTool> tools;
    for (const auto& item : *items) {
        tools.push_back(m_parser.tool(item));
    }
    return tools;
}

Result<McpCallResult> McpClient::callTool(const std::string& name, const Json& arguments, const McpRequestOptions& options) {
    Json params = Json{{"name", name}};
    if (!arguments.is_null()) {
        params["arguments"] = arguments;
    }
    auto raw = request("tools/call", params, options);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return m_parser.callResult(*raw);
}

Result<std::vector<Json>> McpClient::listResources(const McpRequestOptions& options) {
    return listAll("resources/list", "resources", "resource", options);
}

Result<std::vector<Json>> McpClient::listResourceTemplates(const McpRequestOptions& options) {
    auto items = listAll("resources/templates/list", "resourceTemplates", "resourceTemplate", options);
    if (!items && m_codec.rpcNumber(items.error().code) == McpMessageCodec::MethodNotFound) {
        return std::vector<Json>{};
    }
    return items;
}

Result<Json> McpClient::readResource(const std::string& uri, const McpRequestOptions& options) {
    auto raw = request("resources/read", Json{{"uri", uri}}, options);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return m_parser.readResult(*raw);
}
