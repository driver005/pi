export module pi.mcp.mcp_server_connection;

import std;
export import pi.mcp.i_mcp_connector;
export import pi.mcp.i_mcp_resource_server;
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
 * packages/coding-agent/src/extensions/mcp/runtime.ts (no OAuth sign-in or logging). Also the resources of the server (IMcpResourceServer); `hasResources()` tells whether it announced the capability.
 */
export class McpServerConnection : public IMcpToolCaller, public IMcpResourceServer {
public:
    using Listener = std::function<void(McpServerConnection&)>;

    McpServerConnection(McpServerConfig config, std::string cwd, std::string clientVersion, IMcpConnector& connector, ISleeper& sleeper)
        : m_config(std::move(config)),
          m_cwd(std::move(cwd)),
          m_clientVersion(std::move(clientVersion)),
          m_connector(connector),
          m_sleeper(sleeper) {}

    ~McpServerConnection() override {
        close();
    }

    /** Called after the tool list changed (connect and list_changed refreshes), outside any lock. */
    void setToolsListener(Listener listener) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_toolsListener = std::move(listener);
    }

    /** Called when state, error or tools change, outside any lock. */
    void setChangeListener(Listener listener) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_changeListener = std::move(listener);
    }

    /** Connects when not connected; concurrent callers share one attempt. */
    Result<void> connect() {
        auto client = acquire();
        if (!client) {
            return std::unexpected(client.error());
        }
        return {};
    }

    Result<McpCallResult> callTool(const std::string& name, const Json& arguments, const McpRequestOptions& options) override {
        return withClient([&](IMcpClient& client) { return client.callTool(name, arguments, options); });
    }

    std::string serverName() const override {
        return m_config.name;
    }

    std::int64_t requestTimeoutMs() const override {
        return timeoutMs();
    }

    Result<McpPage> resourcesPage(const std::optional<std::string>& cursor, const McpRequestOptions& options) override {
        return withClient([&](IMcpClient& client) { return client.listResourcesPage(cursor, options); });
    }

    Result<McpPage> resourceTemplatesPage(const std::optional<std::string>& cursor, const McpRequestOptions& options) override {
        return withClient([&](IMcpClient& client) { return client.listResourceTemplatesPage(cursor, options); });
    }

    Result<std::vector<Json>> allResources(const McpRequestOptions& options) override {
        return withClient([&](IMcpClient& client) { return client.listResources(options); });
    }

    Result<std::vector<Json>> allResourceTemplates(const McpRequestOptions& options) override {
        return withClient([&](IMcpClient& client) { return client.listResourceTemplates(options); });
    }

    Result<Json> readResource(const std::string& uri, const McpRequestOptions& options) override {
        return withClient([&](IMcpClient& client) { return client.readResource(uri, options); });
    }

    /** Whether the server announced the resources capability when it last connected. */
    bool hasResources() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_hasResources;
    }

    /** Disconnects for good. Must not be called from a listener. */
    void close() {
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

    const McpServerConfig& config() const {
        return m_config;
    }

    std::vector<McpTool> tools() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_tools;
    }

    McpServerState state() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    std::string error() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_error;
    }

    std::optional<std::string> instructions() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_instructions;
    }

    std::int64_t timeoutMs() const {
        return static_cast<std::int64_t>(m_config.timeoutSeconds.value_or(60.0) * 1000.0);
    }

private:
    /** Runs `call` on the connected client, connecting first; a lost session retries once and a sign-in challenge is reported. */
    template <typename Call>
    auto withClient(const Call& call) -> decltype(call(std::declval<IMcpClient&>())) {
        for (int attempt = 1;; ++attempt) {
            auto client = acquire();
            if (!client) {
                return std::unexpected(client.error());
            }
            auto result = call(**client);
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

    Result<std::shared_ptr<IMcpClient>> acquire() {
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

    Result<std::shared_ptr<IMcpClient>> open() {
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

    Result<std::shared_ptr<IMcpClient>> connectOnce() {
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
            m_hasResources = client->serverCapabilities().is_object() && client->serverCapabilities().contains("resources");
            m_state = McpServerState::Connected;
            m_error.clear();
        }
        notifyTools();
        notifyChange();
        return client;
    }

    Error connectFailed(const Error& error) {
        if (error.code == "auth_required" && !isClosed()) {
            setState(McpServerState::NeedsAuth, "");
            return Error{"auth_required", signInMessage()};
        }
        const bool closed = isClosed();
        setState(closed ? McpServerState::Closed : McpServerState::Failed, error.message);
        return Error{error.code, "MCP server \"" + m_config.name + "\" failed to connect: " + error.message};
    }

    bool transientError(const Error& error) const {
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

    void refreshTools(IMcpClient* client) {
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

    void handleClientClose(IMcpClient* client) {
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

    void dropClient(const std::shared_ptr<IMcpClient>& client) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_client == client) {
            m_retired.push_back(std::move(m_client));
            m_client.reset();
        }
    }

    void setState(McpServerState state, const std::string& error) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_state = state;
            m_error = error;
        }
        notifyChange();
    }

    void notifyChange() {
        Listener listener;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            listener = m_changeListener;
        }
        if (listener) {
            listener(*this);
        }
    }

    void notifyTools() {
        Listener listener;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            listener = m_toolsListener;
        }
        if (listener) {
            listener(*this);
        }
    }

    bool isClosed() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    Json rootsFor(const std::string& cwd) const {
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

    /** HTTP servers authenticate with OAuth unless the configuration brings an `Authorization` header or `auth.provider`. */
    bool usesOauth() const {
        if (!m_config.http || m_config.authProvider) {
            return false;
        }
        return std::none_of(m_config.headers.begin(), m_config.headers.end(), [](const auto& header) {
            std::string name = header.first;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name == "authorization";
        });
    }

    std::string signInMessage() const {
        if (m_config.authProvider) {
            return "MCP server \"" + m_config.name + "\" requires sign-in with the \"" + *m_config.authProvider +
                   "\" provider credentials.";
        }
        if (usesOauth()) {
            return "MCP server \"" + m_config.name +
                   "\" requires OAuth sign-in. Sign in with the pi CLI (/mcp), which stores the credentials in mcp-auth.json in the agent directory.";
        }
        return "MCP server \"" + m_config.name + "\" requires authentication. Set an Authorization header in mcp.json.";
    }

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
    bool m_hasResources = false;
    bool m_closed = false;
    Listener m_toolsListener;
    Listener m_changeListener;
};
