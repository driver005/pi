export module pi.coding_session_handle;

import std;
export import pi.coding_services;
export import pi.session.i_session_runtime_handle;
export import pi.types.coding_startup_options;
export import pi.types.session_runtime_request;
import pi.mcp_connector;
import pi.mcp_server_manager;
import pi.plugin.plugin_host;
import pi.session.agent_session;
import pi.session.embedded_export_assets;
import pi.session.resource_loader;
import pi.session.settings_manager;
import pi.support.bash_command_executor;
import pi.support.branch_selection_resolver;
import pi.support.hook_bus;
import pi.support.mcp_config_loader;
import pi.support.mcp_oauth_providers;
import pi.support.mcp_result_converter;
import pi.support.mcp_tool_namer;
import pi.support.package_manager;
import pi.support.plugin_resource_loader;
import pi.support.plugin_session_bridge;
import pi.support.plugin_session_events;
import pi.support.plugin_discovery;
import pi.support.model_selector;
import pi.tools.bash_tool;
import pi.tools.edit_tool;
import pi.tools.file_mutation_queue;
import pi.tools.find_tool;
import pi.tools.grep_tool;
import pi.tools.ls_tool;
import pi.tools.read_tool;
import pi.tools.tool_registry;
import pi.tools.write_tool;

/**
 * One live coding session and the services it owns: settings for its cwd (project settings only
 * once the project is trusted), the resource loader, the built-in tools and the AgentSession over
 * them. Problems that do not stop the session (a settings file that does not parse, no model with
 * credentials) are reported as diagnostics.
 */
export class CodingSessionHandle : public ISessionRuntimeHandle {
public:
    CodingSessionHandle(SessionRuntimeRequest request, CodingServices& services, const CodingStartupOptions& options)
        : m_services(services),
          m_cwd(request.cwd),
          m_agentDir(request.agentDir),
          m_manager(std::move(request.sessionManager)),
          m_settings(m_agentDir + "/settings.json", m_cwd + "/.pi/settings.json", false, services.platform().files(), services.platform().locks(), services.platform().ids()),
          m_resources(resourceOptions(options), m_settings, services.platform().files()),
          m_queue(services.platform().files()),
          m_pluginResources(m_resources, m_sessionEvents, m_cwd, [this](const std::vector<std::string>& skills, const std::vector<std::string>& prompts) { m_resources.addPaths(skills, prompts); }),
          m_pluginDiscovery(services.platform().files()),
          m_plugins(services.platform().libraries(), m_tools, m_hooks, services.platform().processes(), services.platform().logger(), PluginContext{request.cwd, request.agentDir}, &services.models().models(), nullptr, &services.platform().clock()),
          m_bash(services.platform().processes(), services.platform().files(), services.platform().crypto(), services.platform().environment()),
          m_configValues(services.platform().environment(), services.platform().processes()),
          m_mcpConfigs(services.platform().files()),
          m_mcpNamer(services.platform().crypto()),
          m_mcpResults(services.platform().files(), services.platform().crypto(), services.platform().base64(), services.platform().environment()),
          m_oauthStore(m_agentDir + "/mcp-auth.json", m_agentDir, services.platform().files(), services.platform().locks(), services.platform().crypto()),
          m_oauthRefresher(services.platform().http(), services.platform().clock(), services.platform().base64()),
          m_oauth(m_oauthStore, m_oauthRefresher, services.platform().clock(), m_configValues),
          m_mcpConnector(services.platform().children(), services.platform().http(), services.platform().sleeper(), services.platform().files(), m_configValues, [this](const std::string& provider) { return providerToken(provider); }, &m_oauth) {
        resolveTrust(options);
        registerTools();
        loadPackages(options);
        loadPlugins(options);
        startMcp(options);
        if (auto loaded = m_pluginResources.reload(); !loaded) {
            warn(loaded.error().message);
        }
        applyPluginFlags(options, true);
        createSession(options);
        m_bridge = std::make_unique<PluginSessionBridge>(*m_session, *m_manager, m_settings, m_services.models().models(), [this] { return mcpServers(); }, nullptr);
        m_plugins.setSessionBridge(m_bridge.get());
        if (!activeTools(options).has_value()) {
            if (m_mcp) {
                m_mcp->setToolsListener([this](const std::vector<std::string>& added) { activateExtraTools(added); });
            }
            activateExtraTools(extraToolNames());
        }
        m_hooks.emit("session_start", Json{{"type", "session_start"}, {"cwd", m_cwd}});
        for (const auto& error : m_settings.drainErrors()) {
            warn("Settings (" + error.scope + "): " + error.message);
        }
    }

    ~CodingSessionHandle() override {
        m_hooks.emit("session_shutdown", Json{{"type", "session_shutdown"}});
        m_plugins.setSessionBridge(nullptr);
        // The listeners and tools below use the session and the registry: stop them first.
        if (m_mcp) {
            m_mcp->close();
        }
        m_plugins.shutdown();
    }

    IAgentSession& session() override {
        return *m_session;
    }

    ISessionManager& sessionManager() override {
        return *m_manager;
    }

    std::string cwd() const override {
        return m_cwd;
    }

    std::string agentDir() const override {
        return m_agentDir;
    }

    std::vector<RuntimeDiagnostic> diagnostics() const override {
        return m_diagnostics;
    }

    bool allowSwitch(const std::string& reason, const std::optional<std::string>& targetSessionFile) override {
        return m_sessionEvents.allowSwitch(reason, targetSessionFile);
    }

    bool allowFork(const std::string& entryId, ForkPosition position) override {
        return m_sessionEvents.allowFork(entryId, position == ForkPosition::At ? "at" : "before");
    }

    std::unique_ptr<ISessionManager> releaseSessionManager() override {
        return std::move(m_manager);
    }

private:
    void resolveTrust(const CodingStartupOptions& options) {
        const std::string policy = m_settings.view().defaultProjectTrust();
        // A project_trust handler belongs to a plugin that loads before the project is trusted: the global ones load for it.
        const auto trusted = m_services.trust().resolve(m_cwd, options.trustProject, policy, [this, &options](const std::string& cwd) {
            loadGlobalPlugins(options);
            return m_sessionEvents.projectTrust(cwd);
        });
        if (!trusted) {
            warn("Could not read project trust: " + trusted.error().message);
            return;
        }
        if (*trusted) {
            m_settings.setProjectTrusted(true);
        }
    }

    void registerTools() {
        PlatformServices& platform = m_services.platform();
        const SettingsView view = m_settings.view();
        addBuiltin(std::make_shared<ReadTool>(platform.files(), platform.base64(), m_cwd));
        addBuiltin(std::make_shared<BashTool>(platform.processes(), platform.files(), platform.crypto(),
                                               platform.environment(), m_cwd, view.shellPath().value_or(""),
                                               view.shellCommandPrefix().value_or("")));
        addBuiltin(std::make_shared<EditTool>(platform.files(), m_queue, m_cwd));
        addBuiltin(std::make_shared<WriteTool>(platform.files(), m_queue, m_cwd));
        addBuiltin(std::make_shared<GrepTool>(platform.processes(), platform.files(), m_cwd));
        addBuiltin(std::make_shared<FindTool>(platform.files(), m_cwd));
        addBuiltin(std::make_shared<LsTool>(platform.files(), m_cwd));
    }

    /** A built-in tool does not replace a plugin's tool of the same name (plugins that load before the trust question). */
    void addBuiltin(std::shared_ptr<ITool> tool) {
        if (m_tools.find(tool->definition().name) == nullptr) {
            m_tools.add(std::move(tool));
        }
    }

    ResourceLoaderOptions resourceOptions(const CodingStartupOptions& options) const {
        ResourceLoaderOptions out;
        out.cwd = m_cwd;
        out.agentDir = m_agentDir;
        out.systemPrompt = options.systemPrompt;
        out.appendSystemPrompt = options.appendSystemPrompt;
        out.noContextFiles = options.noContextFiles;
        out.noSkills = options.noSkills;
        out.noPromptTemplates = options.noPromptTemplates;
        out.additionalSkillPaths = options.skillPaths;
        out.additionalPromptTemplatePaths = options.promptTemplatePaths;
        return out;
    }

    ModelChoice chooseModel(const CodingStartupOptions& options) {
        IModelRuntime& models = m_services.models().models();
        const SessionContext context = m_manager->buildSessionContext();
        ModelSelectionInput input;
        input.available = models.availableModels();
        input.all = models.models();
        // A virtual selection holds across the responses of the physical models it routed to.
        input.saved = m_selection.resolve(m_manager->branchPath(), [&models](const std::string& provider, const std::string& id) { return models.find(provider, id); });
        if (!input.saved) {
            input.saved = context.model;
        }
        input.savedThinking = context.model ? std::optional<std::string>(context.thinkingLevel) : std::nullopt;
        input.requestedModel = options.model;
        input.requestedThinking = options.thinking;
        return m_selector.select(input, m_settings.view());
    }

    std::optional<std::vector<std::string>> activeTools(const CodingStartupOptions& options) const {
        if (options.noTools) {
            return std::vector<std::string>{};
        }
        if (options.tools) {
            return options.tools;
        }
        return m_settings.view().defaultTools();
    }

    void createSession(const CodingStartupOptions& options) {
        PlatformServices& platform = m_services.platform();
        ModelChoice choice = chooseModel(options);
        for (const auto& message : choice.warnings) {
            warn(message);
        }
        AgentSessionConfig config{m_services.agents(),
                                  *m_manager,
                                  m_settings,
                                  m_services.models().models(),
                                  m_pluginResources,
                                  m_tools,
                                  m_bash,
                                  platform.files(),
                                  platform.clock(),
                                  platform.ids(),
                                  platform.sleeper(),
                                  choice.model.value_or(Model{}),
                                  choice.thinkingLevel,
                                  m_cwd,
                                  activeTools(options),
                                  std::nullopt,
                                  {},
                                  {}};
        config.hooks = &m_hooks;
        config.telemetryEnv = platform.environment().get("PI_TELEMETRY");
        config.environment = &platform.environment();
        config.agentDir = m_agentDir;
        config.system = &platform.system();
        config.http = &platform.http();
        config.processes = &platform.processes();
        config.plugins = [this] { return m_plugins.loaded(); };
        config.commands = &m_plugins;
        config.exportAssets = &m_exportAssets;
        config.base64 = &platform.base64();
        m_session = std::make_unique<AgentSession>(std::move(config));
    }

    /** Skills and prompt templates of the configured packages join the resource loader; plugins load with the others. */
    void loadPackages(const CodingStartupOptions& options) {
        PlatformServices& platform = m_services.platform();
        PackageManager packages(platform.files(), platform.processes(), m_settings, m_cwd, m_agentDir);
        PackagePaths found = packages.load(true);
        for (const std::string& problem : found.warnings) {
            warn(problem);
        }
        m_resources.addPaths(options.noSkills ? std::vector<std::string>{} : found.skills, options.noPromptTemplates ? std::vector<std::string>{} : found.prompts);
        m_packagePlugins = std::move(found.plugins);
    }

    /** The plugins that do not depend on the project: <agent-dir>/plugins and --plugin. Loaded once, before the trust question when a plugin may answer it. */
    void loadGlobalPlugins(const CodingStartupOptions& options) {
        if (options.noPlugins || m_globalPluginsLoaded) {
            return;
        }
        m_globalPluginsLoaded = true;
        std::vector<std::string> paths = m_pluginDiscovery.discover(m_agentDir + "/plugins");
        paths.insert(paths.end(), options.pluginPaths.begin(), options.pluginPaths.end());
        loadPluginPaths(paths);
        applyPluginFlags(options, false);
    }

    void loadPlugins(const CodingStartupOptions& options) {
        if (options.noPlugins) {
            return;
        }
        loadGlobalPlugins(options);
        std::vector<std::string> paths;
        if (m_settings.projectTrusted()) {
            paths = m_pluginDiscovery.discover(m_cwd + "/.pi/plugins");
        }
        paths.insert(paths.end(), m_packagePlugins.begin(), m_packagePlugins.end());
        loadPluginPaths(paths);
    }

    void loadPluginPaths(const std::vector<std::string>& paths) {
        for (const Error& error : m_plugins.load(paths)) {
            warn("Plugin: " + error.message);
        }
    }

    /** Command line flags plugins declared get their values; the others are reported (a plugin may be missing or disabled). */
    void applyPluginFlags(const CodingStartupOptions& options, bool reportUnknown) {
        for (const auto& [name, value] : options.pluginFlags) {
            if (const auto set = m_plugins.setFlag(name, value); !set && reportUnknown) {
                warn(set.error().message);
            }
        }
    }

    Json mcpServers() const {
        Json out = Json::array();
        if (!m_mcp) {
            return out;
        }
        for (const McpServerStatus& status : m_mcp->status()) {
            out.push_back(Json{{"name", status.name}, {"state", mcpStateName(status.state)}, {"error", status.error}, {"toolCount", status.toolCount}});
        }
        return out;
    }

    std::string mcpStateName(McpServerState state) const {
        switch (state) {
        case McpServerState::Disabled: return "disabled";
        case McpServerState::Connecting: return "connecting";
        case McpServerState::Connected: return "connected";
        case McpServerState::Disconnected: return "disconnected";
        case McpServerState::NeedsAuth: return "needs_auth";
        case McpServerState::Failed: return "failed";
        case McpServerState::Closed: return "closed";
        }
        return "unknown";
    }

    void startMcp(const CodingStartupOptions& options) {
        if (options.noMcp || activeTools(options).has_value()) {
            return;
        }
        McpConfigLoadOptions load;
        load.agentDir = m_agentDir;
        load.cwd = m_cwd;
        load.projectTrusted = m_settings.projectTrusted();
        const McpConfigResult config = m_mcpConfigs.load(load);
        for (const std::string& error : config.errors) {
            warn("MCP config: " + error);
        }
        if (config.servers.empty()) {
            return;
        }
        PlatformServices& platform = m_services.platform();
        m_mcp = std::make_unique<McpServerManager>(m_tools, m_mcpConnector, platform.sleeper(), m_mcpNamer,
                                                   m_mcpResults, "0");
        m_mcp->start(config.servers, m_cwd, std::chrono::milliseconds(options.mcpStartupWaitMs));
        for (const McpServerStatus& status : m_mcp->status()) {
            if (status.state == McpServerState::Failed || status.state == McpServerState::NeedsAuth) {
                warn("MCP server \"" + status.name + "\": " +
                     (status.error.empty() ? "needs authentication" : status.error));
            }
        }
    }

    void activateExtraTools(const std::vector<std::string>& names) {
        std::vector<std::string> active = m_session->activeToolNames();
        for (const std::string& name : names) {
            if (std::find(active.begin(), active.end(), name) == active.end()) {
                active.push_back(name);
            }
        }
        m_session->setActiveToolsByName(active);
    }

    std::vector<std::string> extraToolNames() const {
        static constexpr std::array<std::string_view, 7> builtin{"read", "bash", "edit", "write", "grep", "find", "ls"};
        std::vector<std::string> names;
        for (const auto& tool : m_tools.all()) {
            const std::string& name = tool->definition().name;
            if (std::find(builtin.begin(), builtin.end(), name) == builtin.end()) {
                names.push_back(name);
            }
        }
        return names;
    }

    std::optional<std::string> providerToken(const std::string& provider) {
        const auto auth = m_services.models().models().getAuth(provider, std::nullopt, {});
        if (!auth || !*auth) {
            return std::nullopt;
        }
        return (*auth)->auth.apiKey;
    }

    void warn(const std::string& message) {
        m_diagnostics.push_back(RuntimeDiagnostic{"warning", message});
    }

    CodingServices& m_services;
    std::string m_cwd;
    std::string m_agentDir;
    std::unique_ptr<ISessionManager> m_manager;
    std::vector<RuntimeDiagnostic> m_diagnostics;
    SettingsManager m_settings;
    ResourceLoader m_resources;
    FileMutationQueue m_queue;
    ToolRegistry m_tools;
    HookBus m_hooks;
    EmbeddedExportAssets m_exportAssets;
    PluginSessionEvents m_sessionEvents{m_hooks};
    PluginResourceLoader m_pluginResources;
    PluginDiscovery m_pluginDiscovery;
    std::vector<std::string> m_packagePlugins;
    bool m_globalPluginsLoaded = false;
    PluginHost m_plugins;
    BashCommandExecutor m_bash;
    ModelSelector m_selector;
    BranchSelectionResolver m_selection;
    ConfigValueResolver m_configValues;
    McpConfigLoader m_mcpConfigs;
    McpToolNamer m_mcpNamer;
    McpResultConverter m_mcpResults;
    McpOauthStore m_oauthStore;
    McpOauthRefresher m_oauthRefresher;
    McpOauthProviders m_oauth;
    McpConnector m_mcpConnector;
    std::unique_ptr<McpServerManager> m_mcp;
    std::unique_ptr<AgentSession> m_session;
    std::unique_ptr<PluginSessionBridge> m_bridge;
};

/** Loads plugins from <agent-dir>/plugins, the trusted project's .pi/plugins and --plugin. */

/** Connects the servers of mcp.json. Skipped when the tool set is restricted, since MCP tools are not in it. */

/** Tools beyond the built-in set (from plugins and MCP servers). */
