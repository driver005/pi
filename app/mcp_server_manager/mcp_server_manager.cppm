module;

#include <nlohmann/json.hpp>

export module pi.mcp_server_manager;

import std;
export import pi.mcp.i_mcp_connector;
export import pi.mcp.i_mcp_server_manager;
export import pi.platform.i_sleeper;
export import pi.support.mcp_result_converter;
export import pi.support.mcp_server_config_validator;
export import pi.support.mcp_tool_namer;
export import pi.tool.i_tool_registry;
import pi.mcp.mcp_server_connection;
import pi.mcp.mcp_tool_adapter;

/**
 * Connects the configured MCP servers in parallel and keeps their tools registered. Tool names are
 * `mcp__<server>__<tool>`; names stay unique and stable across list refreshes. Exposure: `hidden`
 * tools are not registered, every other mode registers the tool directly (codemode and tool search
 * are not ported). Port of the connection and tool handling in
 * packages/coding-agent/src/extensions/mcp/index.ts.
 */
export class McpServerManager : public IMcpServerManager {
public:
    McpServerManager(IToolRegistry& registry, IMcpConnector& connector, ISleeper& sleeper,
                     const McpToolNamer& namer, McpResultConverter& converter, std::string clientVersion);
    ~McpServerManager() override;

    void start(const std::vector<McpServerConfig>& servers, const std::string& cwd,
               std::chrono::milliseconds startupWait) override;
    std::vector<McpServerStatus> status() const override;
    void close() override;

private:
    bool hasVisibleTools(const McpServerConfig& config) const;
    void connectInBackground(McpServerConnection& connection);
    void registerTools(McpServerConnection& connection);
    std::string assignName(const std::string& server, const std::string& tool,
                           const std::vector<std::string>& plain, std::set<std::string>& current);
    void removeTools(const std::string& server);
    McpServerStatus statusOf(const McpServerConnection& connection) const;

    IToolRegistry& m_registry;
    IMcpConnector& m_connector;
    ISleeper& m_sleeper;
    const McpToolNamer& m_namer;
    McpResultConverter& m_converter;
    std::string m_clientVersion;
    McpServerConfigValidator m_validator;
    mutable std::mutex m_mutex;
    std::condition_variable m_settled;
    std::size_t m_unsettled = 0;
    bool m_closed = false;
    std::vector<std::unique_ptr<McpServerConnection>> m_connections;
    std::vector<McpServerStatus> m_inactive;
    std::vector<std::thread> m_workers;
    /** Tool name to the `<server>\0<tool>` that owns it. */
    std::map<std::string, std::string> m_owners;
    /** Tool names currently registered per server. */
    std::map<std::string, std::set<std::string>> m_registered;
};

McpServerManager::McpServerManager(IToolRegistry& registry, IMcpConnector& connector,
                                   ISleeper& sleeper, const McpToolNamer& namer,
                                   McpResultConverter& converter, std::string clientVersion)
    : m_registry(registry),
      m_connector(connector),
      m_sleeper(sleeper),
      m_namer(namer),
      m_converter(converter),
      m_clientVersion(std::move(clientVersion)) {}

McpServerManager::~McpServerManager() {
    close();
}

bool McpServerManager::hasVisibleTools(const McpServerConfig& config) const {
    if (config.exposure != McpExposure::Hidden) {
        return true;
    }
    return std::any_of(config.toolExposure.begin(), config.toolExposure.end(),
                       [](const auto& entry) { return entry.second != McpExposure::Hidden; });
}

void McpServerManager::start(const std::vector<McpServerConfig>& servers, const std::string& cwd,
                             std::chrono::milliseconds startupWait) {
    std::vector<McpServerConnection*> started;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return;
        }
        for (const McpServerConfig& config : servers) {
            if (!config.enabled || !hasVisibleTools(config)) {
                McpServerStatus inactive;
                inactive.name = config.name;
                inactive.state = McpServerState::Disabled;
                inactive.error = config.enabled ? "all tools are hidden" : "disabled";
                m_inactive.push_back(inactive);
                continue;
            }
            auto connection = std::make_unique<McpServerConnection>(config, cwd, m_clientVersion,
                                                                    m_connector, m_sleeper);
            connection->setToolsListener([this](McpServerConnection& changed) { registerTools(changed); });
            started.push_back(connection.get());
            m_connections.push_back(std::move(connection));
        }
        m_unsettled += started.size();
    }
    for (McpServerConnection* connection : started) {
        connectInBackground(*connection);
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_settled.wait_for(lock, startupWait, [this]() { return m_unsettled == 0; });
}

void McpServerManager::connectInBackground(McpServerConnection& connection) {
    std::thread worker([this, &connection]() {
        connection.connect();
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            --m_unsettled;
        }
        m_settled.notify_all();
    });
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_workers.push_back(std::move(worker));
}

/** Like Codex, all tools whose names sanitize alike get the hash suffix, so the plain name does not depend on list order. */
std::string McpServerManager::assignName(const std::string& server, const std::string& tool,
                                         const std::vector<std::string>& plain,
                                         std::set<std::string>& current) {
    const std::string owner = server + std::string(1, '\0') + tool;
    const std::string name = m_namer.create(server, tool, [&](const std::string& candidate) {
        const auto existing = m_owners.find(candidate);
        return (existing != m_owners.end() && existing->second != owner) || current.contains(candidate) ||
               std::count(plain.begin(), plain.end(), candidate) > 1;
    });
    m_owners[name] = owner;
    current.insert(name);
    return name;
}

void McpServerManager::registerTools(McpServerConnection& connection) {
    const McpServerConfig& config = connection.config();
    const std::vector<McpTool> tools = connection.tools();
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed) {
        return;
    }
    std::set<std::string> unique;
    std::vector<std::string> plain;
    for (const McpTool& tool : tools) {
        if (unique.insert(tool.name).second) {
            plain.push_back(m_namer.create(config.name, tool.name));
        }
    }
    std::set<std::string> current;
    for (const McpTool& tool : tools) {
        const std::string name = assignName(config.name, tool.name, plain, current);
        if (m_validator.toolExposure(config, tool.name) == McpExposure::Hidden) {
            current.erase(name);
            continue;
        }
        m_registry.add(std::make_shared<McpToolAdapter>(config.name, tool, name, connection, m_converter,
                                                        connection.timeoutMs()));
    }
    for (const std::string& name : m_registered[config.name]) {
        if (!current.contains(name)) {
            m_registry.remove(name);
        }
    }
    m_registered[config.name] = std::move(current);
}

void McpServerManager::removeTools(const std::string& server) {
    for (const std::string& name : m_registered[server]) {
        m_registry.remove(name);
    }
    m_registered[server].clear();
}

McpServerStatus McpServerManager::statusOf(const McpServerConnection& connection) const {
    McpServerStatus status;
    status.name = connection.config().name;
    status.state = connection.state();
    status.error = connection.error();
    const auto found = m_registered.find(status.name);
    status.toolCount = found == m_registered.end() ? 0 : found->second.size();
    return status;
}

std::vector<McpServerStatus> McpServerManager::status() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<McpServerStatus> out = m_inactive;
    for (const auto& connection : m_connections) {
        out.push_back(statusOf(*connection));
    }
    return out;
}

void McpServerManager::close() {
    std::vector<std::thread> workers;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return;
        }
        m_closed = true;
        workers = std::move(m_workers);
        m_workers.clear();
    }
    for (const auto& connection : m_connections) {
        connection->close();
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& connection : m_connections) {
        removeTools(connection->config().name);
    }
}
