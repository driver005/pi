export module pi.durable_toolbox;

import std;
export import pi.coding_services;
export import pi.session.settings_manager;
export import pi.tool.i_tool;
export import pi.types.coding_startup_options;
import pi.mcp_connector;
import pi.mcp_server_manager;
import pi.plugin.plugin_host;
import pi.support.config_value_resolver;
import pi.support.hook_bus;
import pi.support.mcp_config_loader;
import pi.support.mcp_result_converter;
import pi.support.mcp_tool_namer;
import pi.support.plugin_discovery;
import pi.tools.tool_registry;

/**
 * The tools beyond the built-in ones that durable sessions of one working directory offer: those of the plugins found in
 * `<agent-dir>/plugins`, the trusted project's `.pi/plugins` and `--plugin`, and those of the servers of `mcp.json`. Plugins
 * load and MCP servers connect when the toolbox is made (startup waits at most `mcpStartupWaitMs` for the servers); a server
 * that connects later adds its tools through the change listener. Plugin hooks (`tool_call`, `tool_result`, `context`, `message_end`, `turn_end`) reach durable sessions through `hooks()`
 * (see PluginHookExtension); plugin commands are not used. Problems that do not stop the toolbox are diagnostics.
 */
export class DurableToolbox {
public:
    using ChangeListener = std::function<void()>;

    DurableToolbox(CodingServices& services, SettingsManager& settings, std::string cwd, std::string agentDir, const CodingStartupOptions& startup)
        : m_services(services),
          m_settings(settings),
          m_startup(startup),
          m_cwd(std::move(cwd)),
          m_agentDir(std::move(agentDir)),
          m_pluginDiscovery(services.platform().files()),
          m_plugins(services.platform().libraries(), m_tools, *m_hooks, services.platform().processes(), services.platform().logger(), PluginContext{m_cwd, m_agentDir}),
          m_configValues(services.platform().environment(), services.platform().processes()),
          m_mcpConfigs(services.platform().files()),
          m_mcpNamer(services.platform().crypto()),
          m_mcpResults(services.platform().files(), services.platform().crypto(), services.platform().base64(), services.platform().environment()),
          m_mcpConnector(services.platform().children(), services.platform().http(), services.platform().sleeper(), services.platform().files(), m_configValues,
                         [this](const std::string& provider) { return providerToken(provider); }) {
        loadPlugins(startup);
        startMcp(startup);
    }

    ~DurableToolbox() {
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
        {
            const std::lock_guard<std::mutex> lock(m_listenerMutex);
            m_listener = listener;
        }
        if (m_mcp) {
            m_mcp->setToolsListener([listener = std::move(listener)](const std::vector<std::string>&) { listener(); });
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
        ChangeListener listener;
        {
            const std::lock_guard<std::mutex> lock(m_listenerMutex);
            listener = m_listener;
        }
        if (listener) {
            listener();
        }
        if (!problems.empty()) {
            return std::unexpected(Error{"plugin", problems.front()});
        }
        return {};
    }

    /** The bus the directory's plugins subscribe to; stays valid after the plugins shut down (then without handlers). */
    std::shared_ptr<IHookBus> hooks() const {
        return m_hooks;
    }

    std::vector<std::string> diagnostics() const {
        const std::lock_guard<std::mutex> lock(m_listenerMutex);
        return m_diagnostics;
    }

private:
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
        paths.insert(paths.end(), startup.pluginPaths.begin(), startup.pluginPaths.end());
        for (const Error& error : m_plugins.load(paths)) {
            addDiagnostic("Plugin: " + error.message);
            problems.push_back(error.message);
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
        if (config.servers.empty()) {
            return;
        }
        PlatformServices& platform = m_services.platform();
        m_mcp = std::make_unique<McpServerManager>(m_tools, m_mcpConnector, platform.sleeper(), m_mcpNamer, m_mcpResults, "0");
        m_mcp->start(config.servers, m_cwd, std::chrono::milliseconds(startup.mcpStartupWaitMs));
        for (const McpServerStatus& status : m_mcp->status()) {
            if (status.state == McpServerState::Failed || status.state == McpServerState::NeedsAuth) {
                addDiagnostic("MCP server \"" + status.name + "\": " + (status.error.empty() ? "needs authentication" : status.error));
            }
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
    McpConnector m_mcpConnector;
    std::unique_ptr<McpServerManager> m_mcp;
    std::mutex m_reloadMutex;
    mutable std::mutex m_listenerMutex;
    ChangeListener m_listener;
};
