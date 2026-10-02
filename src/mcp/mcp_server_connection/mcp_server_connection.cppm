module;

#include <nlohmann/json.hpp>

export module pi.mcp.mcp_server_connection;

import std;
export import pi.mcp.i_mcp_connector;
export import pi.mcp.i_mcp_tool_caller;
export import pi.platform.i_sleeper;
export import pi.support.abort_signal;
export import pi.types.mcp_server_config;
export import pi.types.mcp_server_state;
export import pi.types.mcp_tool;

/**
 * One configured server. Connects on demand, retries transient HTTP failures while connecting,
 * keeps the tool list fresh on notifications/tools/list_changed and reconnects lazily when a call
 * finds the connection gone. Port of McpServerConnection in
 * packages/coding-agent/src/extensions/mcp/runtime.ts (no resources, OAuth sign-in or logging).
 */
export class McpServerConnection : public IMcpToolCaller {
public:
    using Listener = std::function<void(McpServerConnection&)>;

    McpServerConnection(McpServerConfig config, std::string cwd, std::string clientVersion,
                        IMcpConnector& connector, ISleeper& sleeper);
    ~McpServerConnection() override;

    /** Called after the tool list changed (connect and list_changed refreshes), outside any lock. */
    void setToolsListener(Listener listener);
    /** Called when state, error or tools change, outside any lock. */
    void setChangeListener(Listener listener);

    /** Connects when not connected; concurrent callers share one attempt. */
    Result<void> connect();
    Result<McpCallResult> callTool(const std::string& name, const Json& arguments,
                                   const McpRequestOptions& options) override;
    /** Disconnects for good. Must not be called from a listener. */
    void close();

    const McpServerConfig& config() const;
    std::vector<McpTool> tools() const;
    McpServerState state() const;
    std::string error() const;
    std::optional<std::string> instructions() const;
    std::int64_t timeoutMs() const;

private:
    Result<std::shared_ptr<IMcpClient>> acquire();
    Result<std::shared_ptr<IMcpClient>> open();
    Result<std::shared_ptr<IMcpClient>> connectOnce();
    Error connectFailed(const Error& error);
    bool transientError(const Error& error) const;
    void refreshTools(IMcpClient* client);
    void handleClientClose(IMcpClient* client);
    void dropClient(const std::shared_ptr<IMcpClient>& client);
    void setState(McpServerState state, const std::string& error);
    void notifyChange();
    void notifyTools();
    bool isClosed() const;
    Json rootsFor(const std::string& cwd) const;
    std::string signInMessage() const;

    McpServerConfig m_config;
    std::string m_cwd;
    std::string m_clientVersion;
    IMcpConnector& m_connector;
    ISleeper& m_sleeper;
    std::shared_ptr<AbortSignal> m_abort = std::make_shared<AbortSignal>();
    /** Serializes connection attempts. */
    std::mutex m_openMutex;
    mutable std::mutex m_mutex;
    std::shared_ptr<IMcpClient> m_client;
    /** Dropped clients stay alive until close: a client cannot be destroyed from its own callbacks. */
    std::vector<std::shared_ptr<IMcpClient>> m_retired;
    std::vector<McpTool> m_tools;
    std::optional<std::string> m_instructions;
    McpServerState m_state = McpServerState::Connecting;
    std::string m_error;
    bool m_closed = false;
    Listener m_toolsListener;
    Listener m_changeListener;
};

McpServerConnection::McpServerConnection(McpServerConfig config, std::string cwd,
                                         std::string clientVersion, IMcpConnector& connector,
                                         ISleeper& sleeper)
    : m_config(std::move(config)),
      m_cwd(std::move(cwd)),
      m_clientVersion(std::move(clientVersion)),
      m_connector(connector),
      m_sleeper(sleeper) {}

McpServerConnection::~McpServerConnection() {
    close();
}

void McpServerConnection::setToolsListener(Listener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_toolsListener = std::move(listener);
}

void McpServerConnection::setChangeListener(Listener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_changeListener = std::move(listener);
}

const McpServerConfig& McpServerConnection::config() const {
    return m_config;
}

std::vector<McpTool> McpServerConnection::tools() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_tools;
}

McpServerState McpServerConnection::state() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

std::string McpServerConnection::error() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

std::optional<std::string> McpServerConnection::instructions() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_instructions;
}

std::int64_t McpServerConnection::timeoutMs() const {
    return static_cast<std::int64_t>(m_config.timeoutSeconds.value_or(60.0) * 1000.0);
}

bool McpServerConnection::isClosed() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

void McpServerConnection::notifyChange() {
    Listener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        listener = m_changeListener;
    }
    if (listener) {
        listener(*this);
    }
}

void McpServerConnection::notifyTools() {
    Listener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        listener = m_toolsListener;
    }
    if (listener) {
        listener(*this);
    }
}

void McpServerConnection::setState(McpServerState state, const std::string& error) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_state = state;
        m_error = error;
    }
    notifyChange();
}

std::string McpServerConnection::signInMessage() const {
    if (m_config.authProvider) {
        return "MCP server \"" + m_config.name + "\" requires sign-in with the \"" + *m_config.authProvider +
               "\" provider credentials.";
    }
    return "MCP server \"" + m_config.name +
           "\" requires authentication. OAuth sign-in is not supported here; set an Authorization header in mcp.json.";
}

bool McpServerConnection::transientError(const Error& error) const {
    if (error.code == "transport") {
        return true;
    }
    if (!error.code.starts_with("http:")) {
        return false;
    }
    int status = 0;
    const std::string digits = error.code.substr(5);
    std::from_chars(digits.data(), digits.data() + digits.size(), status);
    return status == 408 || status == 429 || (status >= 500 && status != 501);
}

Json McpServerConnection::rootsFor(const std::string& cwd) const {
    std::string uri = "file://";
    for (const char c : cwd) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            uri.push_back(c);
        } else {
            uri += std::format("%{:02X}", byte);
        }
    }
    std::string trimmed = cwd;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    const std::size_t slash = trimmed.rfind('/');
    const std::string name = slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
    return Json::array({Json{{"uri", uri}, {"name", name}}});
}

Error McpServerConnection::connectFailed(const Error& error) {
    if (error.code == "auth_required" && !isClosed()) {
        setState(McpServerState::NeedsAuth, "");
        return Error{"auth_required", signInMessage()};
    }
    const bool closed = isClosed();
    setState(closed ? McpServerState::Closed : McpServerState::Failed, error.message);
    return Error{error.code, "MCP server \"" + m_config.name + "\" failed to connect: " + error.message};
}

Result<std::shared_ptr<IMcpClient>> McpServerConnection::connectOnce() {
    McpClientOptions options;
    options.version = m_clientVersion;
    options.requestTimeoutMs = timeoutMs();
    options.roots = rootsFor(m_cwd);
    auto connected = m_connector.connect(m_config, m_cwd, options);
    if (!connected) {
        return std::unexpected(connected.error());
    }
    std::shared_ptr<IMcpClient> client = std::move(*connected);
    IMcpClient* raw = client.get();
    client->onNotification("notifications/tools/list_changed",
                           [this, raw](const Json&) { refreshTools(raw); });
    client->onClose([this, raw]() { handleClientClose(raw); });
    // Servers without the tools capability (prompts or resources only) do not answer tools/list.
    std::vector<McpTool> listed;
    if (client->serverCapabilities().is_object() && client->serverCapabilities().contains("tools")) {
        auto tools = client->listTools(McpRequestOptions{});
        if (!tools) {
            client->close();
            return std::unexpected(tools.error());
        }
        listed = std::move(*tools);
    }
    if (isClosed()) {
        client->close();
        return std::unexpected(Error{"closed", "shut down while connecting"});
    }
    if (!client->connected()) {
        return std::unexpected(Error{"closed", "connection closed during setup"});
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_client = client;
        m_tools = std::move(listed);
        m_instructions = client->instructions();
        m_state = McpServerState::Connected;
        m_error.clear();
    }
    notifyTools();
    notifyChange();
    return client;
}

Result<std::shared_ptr<IMcpClient>> McpServerConnection::open() {
    setState(McpServerState::Connecting, "");
    static constexpr std::array<std::int64_t, 2> retryDelaysMs{250, 1000};
    for (std::size_t attempt = 0;; ++attempt) {
        auto client = connectOnce();
        if (client) {
            return client;
        }
        const bool retry = m_config.http && attempt < retryDelaysMs.size() && !isClosed() &&
                           transientError(client.error());
        if (!retry || !m_sleeper.sleep(std::chrono::milliseconds(retryDelaysMs[attempt]), m_abort) ||
            isClosed()) {
            return std::unexpected(connectFailed(client.error()));
        }
    }
}

Result<std::shared_ptr<IMcpClient>> McpServerConnection::acquire() {
    const std::lock_guard<std::mutex> opening(m_openMutex);
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(Error{"closed", "MCP server \"" + m_config.name + "\" is shut down"});
        }
        if (m_client && m_client->connected()) {
            return m_client;
        }
    }
    return open();
}

Result<void> McpServerConnection::connect() {
    auto client = acquire();
    if (!client) {
        return std::unexpected(client.error());
    }
    return {};
}

void McpServerConnection::dropClient(const std::shared_ptr<IMcpClient>& client) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_client == client) {
        m_retired.push_back(std::move(m_client));
        m_client.reset();
    }
}

Result<McpCallResult> McpServerConnection::callTool(const std::string& name, const Json& arguments,
                                                    const McpRequestOptions& options) {
    for (int attempt = 1;; ++attempt) {
        auto client = acquire();
        if (!client) {
            return std::unexpected(client.error());
        }
        auto result = (*client)->callTool(name, arguments, options);
        if (result) {
            return result;
        }
        if (result.error().code == "session_expired" && attempt == 1) {
            // The server no longer knows the session, so it did not run the call: retry on a new one.
            dropClient(*client);
            continue;
        }
        if (result.error().code == "auth_required") {
            dropClient(*client);
            (*client)->close();
            setState(McpServerState::NeedsAuth, "");
            return std::unexpected(Error{"auth_required", signInMessage()});
        }
        return result;
    }
}

void McpServerConnection::refreshTools(IMcpClient* client) {
    auto tools = client->listTools(McpRequestOptions{});
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_client.get() != client || m_closed) {
            return;
        }
        if (tools) {
            m_tools = std::move(*tools);
        } else {
            m_error = "Failed to refresh tools: " + tools.error().message;
        }
    }
    if (tools) {
        notifyTools();
    }
    notifyChange();
}

void McpServerConnection::handleClientClose(IMcpClient* client) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_client.get() != client || m_closed) {
            return;
        }
        m_retired.push_back(std::move(m_client));
        m_client.reset();
        m_state = McpServerState::Disconnected;
        m_error = "Connection closed";
    }
    notifyChange();
}

void McpServerConnection::close() {
    std::vector<std::shared_ptr<IMcpClient>> clients;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return;
        }
        m_closed = true;
        m_state = McpServerState::Closed;
        if (m_client) {
            clients.push_back(std::move(m_client));
            m_client.reset();
        }
        for (auto& retired : m_retired) {
            clients.push_back(std::move(retired));
        }
        m_retired.clear();
    }
    m_abort->abort();
    notifyChange();
    for (const auto& client : clients) {
        client->close();
    }
}
