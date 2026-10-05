export module pi.durable_toolbox;

import std;
export import pi.coding_services;
export import pi.mcp.i_mcp_server_registrar;
export import pi.session.settings_manager;
export import pi.tool.i_tool;
export import pi.types.coding_startup_options;
import pi.mcp_connector;
import pi.mcp_server_manager;
import pi.plugin.plugin_host;
import pi.support.config_value_resolver;
import pi.support.hook_bus;
import pi.support.mcp_config_loader;
import pi.support.mcp_oauth_providers;
import pi.support.mcp_result_converter;
import pi.support.mcp_tool_namer;
import pi.support.package_manager;
import pi.support.plugin_discovery;
import pi.tools.tool_registry;

/**
 * The tools beyond the built-in ones that durable sessions of one working directory offer: those of the plugins found in
 * `<agent-dir>/plugins`, the trusted project's `.pi/plugins` and `--plugin`, and those of the servers of `mcp.json`. Plugins
 * load and MCP servers connect when the toolbox is made (startup waits at most `mcpStartupWaitMs` for the servers); a server
 * that connects later adds its tools through the change listener. Plugin hooks (`tool_call`, `tool_result`, `context`, `message_end`, `turn_end`) reach durable sessions through `hooks()`
 * (see PluginHookExtension); plugin commands are not used. Plugins also register MCP servers through the toolbox (IMcpServerRegistrar):
 * they connect next to those of `mcp.json`, which win on a name clash, and go away with the plugin. Problems that do not stop the toolbox are
 * diagnostics.
 */
export class DurableToolbox : public IMcpServerRegistrar {
public:
    using ChangeListener = std::function<void()>;

    DurableToolbox(CodingServices& services, SettingsManager& settings, std::string cwd, std::string agentDir, const CodingStartupOptions& startup)
        : m_services(services),
          m_settings(settings),
          m_startup(startup),
          m_cwd(std::move(cwd)),
          m_agentDir(std::move(agentDir)),
          m_pluginDiscovery(services.platform().files()),
          m_plugins(services.platform().libraries(), m_tools, *m_hooks, services.platform().processes(), services.platform().logger(), PluginContext{m_cwd, m_agentDir}, &services.models().models(), this, &services.platform().clock()),
          m_configValues(services.platform().environment(), services.platform().processes()),
          m_mcpConfigs(services.platform().files()),
          m_mcpNamer(services.platform().crypto()),
          m_mcpResults(services.platform().files(), services.platform().crypto(), services.platform().base64(), services.platform().environment()),
          m_oauthStore(m_agentDir + "/mcp-auth.json", m_agentDir, services.platform().files(), services.platform().locks(), services.platform().crypto()),
          m_oauthRefresher(services.platform().http(), services.platform().clock(), services.platform().base64()),
          m_oauth(m_oauthStore, m_oauthRefresher, services.platform().clock(), m_configValues),
          m_mcpConnector(services.platform().children(), services.platform().http(), services.platform().sleeper(), services.platform().files(), m_configValues,
                         [this](const std::string& provider) { return providerToken(provider); }, &m_oauth) {
        loadPlugins(startup);
        startMcp(startup);
    }

    ~DurableToolbox() override {
        // The listener and the tools use the registry: stop the servers and plugins first.
        if (m_mcp) {
            m_mcp->close();
        }
        m_plugins.shutdown();
    }

    DurableToolbox(const DurableToolbox&) = delete;
    DurableToolbox& operator=(const DurableToolbox&) = delete;

    /** The tools of plugins and MCP servers registered so far. */
    std::vector<std::shared_ptr<ITool>> tools() const {
        return m_tools.all();
    }

    /** Runs `listener` whenever a server adds tools after startup and after reload(). */
    void onChange(ChangeListener listener) {
        const std::lock_guard<std::mutex> lock(m_listenerMutex);
        m_listener = std::move(listener);
    }

    Result<void> registerServer(const std::string& owner, const std::string& name, const Json& config) override {
        auto validated = m_mcpValidator.validate(name, config);
        if (!validated) {
            return std::unexpected(validated.error());
        }
        bool started = false;
        {
            const std::lock_guard<std::mutex> lock(m_mcpMutex);
            const auto existing = m_pluginServers.find(name);
            if (existing != m_pluginServers.end() && existing->second.first != owner) {
                return std::unexpected(Error{"mcp_server_conflict", "MCP server \"" + name + "\" is already registered by another plugin"});
            }
            validated->source = "plugin";
            validated->scope = "plugin";
            m_pluginServers[name] = {owner, *validated};
            m_pluginServerConfigs[name] = config;
            // Before startup the server is collected with the configured ones; after it, connected right away.
            if (m_mcpStarted && m_mcpAllowed && !m_configuredServers.contains(name)) {
                ensureMcp();
                m_mcp->addServers({*validated}, m_cwd);
            }
            started = m_mcpStarted;
        }
        if (started) {
            emitServersChange();
        }
        return {};
    }

    void unregisterServer(const std::string& owner, const std::string& name) override {
        bool stopped = false;
        {
            const std::lock_guard<std::mutex> lock(m_mcpMutex);
            const auto existing = m_pluginServers.find(name);
            if (existing == m_pluginServers.end() || existing->second.first != owner) {
                return;
            }
            m_pluginServers.erase(existing);
            m_pluginServerConfigs.erase(name);
            if (m_mcp && !m_configuredServers.contains(name)) {
                m_mcp->stopServer(name);
                stopped = true;
            }
        }
        emitServersChange();
        if (stopped) {
            notifyChange();
        }
    }

    /**
     * Unloads the plugins and loads them again from the same places, so plugins added, removed or rebuilt on disk take
     * effect; their tools and hooks are replaced and the change listener runs. Nothing may be executing a plugin's tool
     * meanwhile: the plugin's code is unmapped. Reloads run one at a time. Plugin problems become diagnostics; the
     * result fails only when a plugin could not be loaded, after the others did load.
     */
    Result<void> reload() {
        const std::lock_guard<std::mutex> serial(m_reloadMutex);
        m_plugins.shutdown();
        {
            const std::lock_guard<std::mutex> lock(m_listenerMutex);
            std::erase_if(m_diagnostics, [](const std::string& line) { return line.starts_with("Plugin: "); });
        }
        const std::vector<std::string> problems = loadPlugins(m_startup);
        notifyChange();
        if (!problems.empty()) {
            return std::unexpected(Error{"plugin", problems.front()});
        }
        return {};
    }

    /** The bus the directory's plugins subscribe to; stays valid after the plugins shut down (then without handlers). */
    std::shared_ptr<IHookBus> hooks() const {
        return m_hooks;
    }

    /** The commands the directory's plugins registered (valid for the toolbox's life; empty once the plugins shut down). */
    IPluginCommands& commands() {
        return m_plugins;
    }

    std::vector<std::string> diagnostics() const {
        const std::lock_guard<std::mutex> lock(m_listenerMutex);
        return m_diagnostics;
    }

private:
    /** `mcp_servers_change`: every server the plugins registered, after a change once the plugins are bound. */
    void emitServersChange() {
        Json servers = Json::array();
        {
            const std::lock_guard<std::mutex> lock(m_mcpMutex);
            for (const auto& [name, registered] : m_pluginServers) {
                servers.push_back(Json{{"name", name}, {"extension", registered.first}, {"config", m_pluginServerConfigs[name]}});
            }
        }
        if (m_hooks->hasHandlers("mcp_servers_change")) {
            m_hooks->emit("mcp_servers_change", Json{{"type", "mcp_servers_change"}, {"servers", servers}});
        }
    }

    std::vector<std::string> loadPlugins(const CodingStartupOptions& startup) {
        std::vector<std::string> problems;
        if (startup.noPlugins) {
            return problems;
        }
        std::vector<std::string> paths = m_pluginDiscovery.discover(m_agentDir + "/plugins");
        if (m_settings.projectTrusted()) {
            for (std::string& path : m_pluginDiscovery.discover(m_cwd + "/.pi/plugins")) {
                paths.push_back(std::move(path));
            }
        }
        PlatformServices& platform = m_services.platform();
        PackageManager packages(platform.files(), platform.processes(), m_settings, m_cwd, m_agentDir);
        PackagePaths fromPackages = packages.load(true);
        for (const std::string& problem : fromPackages.warnings) {
            addDiagnostic(problem);
        }
        paths.insert(paths.end(), fromPackages.plugins.begin(), fromPackages.plugins.end());
        paths.insert(paths.end(), startup.pluginPaths.begin(), startup.pluginPaths.end());
        for (const Error& error : m_plugins.load(paths)) {
            addDiagnostic("Plugin: " + error.message);
            problems.push_back(error.message);
        }
        for (const auto& [name, value] : startup.pluginFlags) {
            if (const auto set = m_plugins.setFlag(name, value); !set) {
                addDiagnostic(set.error().message);
            }
        }
        return problems;
    }

    /** Skipped when the tool set is restricted, since MCP tools are not in it. */
    void startMcp(const CodingStartupOptions& startup) {
        if (startup.noMcp || startup.noTools || startup.tools || m_settings.view().defaultTools()) {
            return;
        }
        McpConfigLoadOptions load;
        load.agentDir = m_agentDir;
        load.cwd = m_cwd;
        load.projectTrusted = m_settings.projectTrusted();
        const McpConfigResult config = m_mcpConfigs.load(load);
        for (const std::string& error : config.errors) {
            addDiagnostic("MCP config: " + error);
        }
        std::vector<McpServerConfig> servers = config.servers;
        {
            const std::lock_guard<std::mutex> lock(m_mcpMutex);
            m_mcpAllowed = true;
            m_mcpStarted = true;
            for (const McpServerConfig& configured : config.servers) {
                m_configuredServers.insert(configured.name);
            }
            // Servers of plugins join the configured ones; a configured server of the same name wins.
            for (const auto& [name, registered] : m_pluginServers) {
                if (!m_configuredServers.contains(name)) {
                    servers.push_back(registered.second);
                }
            }
            if (servers.empty()) {
                return;
            }
            ensureMcp();
        }
        m_mcp->start(servers, m_cwd, std::chrono::milliseconds(startup.mcpStartupWaitMs));
        for (const McpServerStatus& status : m_mcp->status()) {
            if (status.state == McpServerState::Failed || status.state == McpServerState::NeedsAuth) {
                addDiagnostic("MCP server \"" + status.name + "\": " + (status.error.empty() ? "needs authentication" : status.error));
            }
        }
    }

    /** Makes the server manager the first time it is needed. The caller holds the MCP lock. */
    void ensureMcp() {
        if (m_mcp) {
            return;
        }
        PlatformServices& platform = m_services.platform();
        m_mcp = std::make_unique<McpServerManager>(m_tools, m_mcpConnector, platform.sleeper(), m_mcpNamer, m_mcpResults, "0");
        m_mcp->setToolsListener([this](const std::vector<std::string>&) { notifyChange(); });
    }

    void notifyChange() {
        ChangeListener listener;
        {
            const std::lock_guard<std::mutex> lock(m_listenerMutex);
            listener = m_listener;
        }
        if (listener) {
            listener();
        }
    }

    void addDiagnostic(const std::string& line) {
        const std::lock_guard<std::mutex> lock(m_listenerMutex);
        m_diagnostics.push_back(line);
    }

    std::optional<std::string> providerToken(const std::string& provider) {
        const auto auth = m_services.models().models().getAuth(provider, std::nullopt, {});
        if (!auth || !*auth) {
            return std::nullopt;
        }
        return (*auth)->auth.apiKey;
    }

    CodingServices& m_services;
    SettingsManager& m_settings;
    CodingStartupOptions m_startup;
    std::string m_cwd;
    std::string m_agentDir;
    std::vector<std::string> m_diagnostics;
    ToolRegistry m_tools;
    std::shared_ptr<HookBus> m_hooks = std::make_shared<HookBus>();
    PluginDiscovery m_pluginDiscovery;
    PluginHost m_plugins;
    ConfigValueResolver m_configValues;
    McpConfigLoader m_mcpConfigs;
    McpToolNamer m_mcpNamer;
    McpResultConverter m_mcpResults;
    McpOauthStore m_oauthStore;
    McpOauthRefresher m_oauthRefresher;
    McpOauthProviders m_oauth;
    McpConnector m_mcpConnector;
    McpServerConfigValidator m_mcpValidator;
    /** Guards the plugin servers and the manager's creation. */
    std::mutex m_mcpMutex;
    bool m_mcpStarted = false;
    bool m_mcpAllowed = false;
    std::set<std::string> m_configuredServers;
    std::map<std::string, std::pair<std::string, McpServerConfig>> m_pluginServers;
    std::map<std::string, Json> m_pluginServerConfigs;
    std::unique_ptr<McpServerManager> m_mcp;
    std::mutex m_reloadMutex;
    mutable std::mutex m_listenerMutex;
    ChangeListener m_listener;
};
