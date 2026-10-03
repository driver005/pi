export module pi.testing.scripted_mcp_transport;

import std;
export import pi.mcp.i_mcp_transport;

/**
 * An in-memory MCP server for tests. Requests the client sends are answered by registered handlers
 * on a separate delivery thread (like a real transport's reader); everything sent is recorded.
 * Server-initiated messages are queued with push().
 */
export class ScriptedMcpTransport : public IMcpTransport {
public:
    using Handler = std::function<Result<Json>(const Json& params)>;

    ScriptedMcpTransport();
    ~ScriptedMcpTransport() override;

    /** Answers `method` with the handler's result, or a JSON-RPC error {code, message}. */
    void onRequest(const std::string& method, Handler handler);
    /** Registers an initialize handler announcing the given capabilities. */
    void answerInitialize(const Json& capabilities = Json::object(), const std::string& instructions = "");
    /** Never answers `method`, so the client's timeout or abort decides. */
    void ignore(const std::string& method);
    /** Delivers a message to the client (notification or server request). */
    void push(const Json& message);
    /** Closes the connection from the server side. */
    void drop();
    /** Makes start() fail. */
    void failStart(const std::string& message);
    /** The next send of a request for `method` fails with this error (once). */
    void failSend(const std::string& method, Error error);

    std::vector<Json> sent() const;
    std::vector<Json> sent(const std::string& method) const;
    bool closed() const;
    int closeCalls() const;

    Result<void> start() override;
    Result<void> send(const Json& message) override;
    void close() override;
    void setMessageListener(MessageListener listener) override;
    void setErrorListener(ErrorListener listener) override;
    void setCloseListener(CloseListener listener) override;
    void setProtocolVersion(const std::string& version) override;

    std::string protocolVersion() const;

private:
    void deliveryLoop();
    void enqueue(std::function<void()> task);

    mutable std::mutex m_mutex;
    std::map<std::string, Handler> m_handlers;
    std::set<std::string> m_ignored;
    std::vector<Json> m_sent;
    MessageListener m_message;
    CloseListener m_close;
    std::string m_protocolVersion;
    std::optional<std::string> m_startFailure;
    std::map<std::string, std::deque<Error>> m_sendFailures;
    bool m_closed = false;
    bool m_closeNotified = false;
    int m_closeCalls = 0;
    std::deque<std::function<void()>> m_queue;
    std::condition_variable m_queued;
    bool m_stop = false;
    std::thread m_worker;
};

ScriptedMcpTransport::ScriptedMcpTransport() {
    m_worker = std::thread([this]() { deliveryLoop(); });
}

ScriptedMcpTransport::~ScriptedMcpTransport() {
    close();
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_queued.notify_all();
    m_worker.join();
}

void ScriptedMcpTransport::deliveryLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queued.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) {
                return;
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

void ScriptedMcpTransport::enqueue(std::function<void()> task) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(task));
    }
    m_queued.notify_one();
}

void ScriptedMcpTransport::onRequest(const std::string& method, Handler handler) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_handlers[method] = std::move(handler);
}

void ScriptedMcpTransport::answerInitialize(const Json& capabilities, const std::string& instructions) {
    onRequest("initialize", [capabilities, instructions](const Json&) -> Result<Json> {
        Json result = Json{{"protocolVersion", "2025-11-25"},
                           {"capabilities", capabilities},
                           {"serverInfo", Json{{"name", "scripted"}, {"version", "1.0"}}}};
        if (!instructions.empty()) {
            result["instructions"] = instructions;
        }
        return result;
    });
}

void ScriptedMcpTransport::ignore(const std::string& method) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_ignored.insert(method);
}

void ScriptedMcpTransport::push(const Json& message) {
    enqueue([this, message]() {
        MessageListener listener;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            listener = m_message;
        }
        if (listener) {
            listener(message);
        }
    });
}

void ScriptedMcpTransport::drop() {
    enqueue([this]() {
        CloseListener listener;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
            if (m_closeNotified) {
                return;
            }
            m_closeNotified = true;
            listener = m_close;
        }
        if (listener) {
            listener();
        }
    });
}

void ScriptedMcpTransport::failStart(const std::string& message) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_startFailure = message;
}

void ScriptedMcpTransport::failSend(const std::string& method, Error error) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_sendFailures[method].push_back(std::move(error));
}

std::vector<Json> ScriptedMcpTransport::sent() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_sent;
}

std::vector<Json> ScriptedMcpTransport::sent(const std::string& method) const {
    std::vector<Json> out;
    for (const auto& message : sent()) {
        if (message.value("method", "") == method) {
            out.push_back(message);
        }
    }
    return out;
}

bool ScriptedMcpTransport::closed() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

int ScriptedMcpTransport::closeCalls() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_closeCalls;
}

std::string ScriptedMcpTransport::protocolVersion() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_protocolVersion;
}

Result<void> ScriptedMcpTransport::start() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_startFailure) {
        return std::unexpected(Error{"start_failed", *m_startFailure});
    }
    return {};
}

Result<void> ScriptedMcpTransport::send(const Json& message) {
    Handler handler;
    bool ignored = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(Error{"closed", "transport closed"});
        }
        m_sent.push_back(message);
        if (message.contains("method") && message.contains("id")) {
            const std::string method = message["method"].get<std::string>();
            auto failure = m_sendFailures.find(method);
            if (failure != m_sendFailures.end() && !failure->second.empty()) {
                Error error = std::move(failure->second.front());
                failure->second.pop_front();
                return std::unexpected(std::move(error));
            }
            ignored = m_ignored.contains(method);
            const auto found = m_handlers.find(method);
            if (found != m_handlers.end()) {
                handler = found->second;
            }
        } else {
            return {};
        }
    }
    if (ignored) {
        return {};
    }
    const Json id = message["id"];
    const Json params = message.contains("params") ? message["params"] : Json();
    const std::string method = message["method"].get<std::string>();
    enqueue([this, id, params, handler, method]() {
        Json reply;
        if (!handler) {
            reply = Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", -32601}, {"message", "Method not found: " + method}}}};
        } else if (auto result = handler(params)) {
            reply = Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", *result}};
        } else {
            const std::string code = result.error().code;
            const int number = code.starts_with("rpc:") ? std::atoi(code.c_str() + 4) : -32603;
            reply = Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", number}, {"message", result.error().message}}}};
        }
        MessageListener listener;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            listener = m_message;
        }
        if (listener) {
            listener(reply);
        }
    });
    return {};
}

void ScriptedMcpTransport::close() {
    CloseListener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_closeCalls;
        m_closed = true;
        if (!m_closeNotified) {
            m_closeNotified = true;
            listener = m_close;
        }
    }
    if (listener) {
        listener();
    }
}

void ScriptedMcpTransport::setMessageListener(MessageListener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_message = std::move(listener);
}

void ScriptedMcpTransport::setErrorListener(ErrorListener) {}

void ScriptedMcpTransport::setCloseListener(CloseListener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_close = std::move(listener);
}

void ScriptedMcpTransport::setProtocolVersion(const std::string& version) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_protocolVersion = version;
}
