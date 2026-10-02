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
import pi.session.resource_loader;
import pi.session.settings_manager;
import pi.support.bash_command_executor;
import pi.support.hook_bus;
import pi.support.mcp_config_loader;
import pi.support.mcp_result_converter;
import pi.support.mcp_tool_namer;
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
    CodingSessionHandle(SessionRuntimeRequest request, CodingServices& services,
                        const CodingStartupOptions& options);
    ~CodingSessionHandle() override;

    IAgentSession& session() override;
    ISessionManager& sessionManager() override;
    std::string cwd() const override;
    std::string agentDir() const override;
    std::vector<RuntimeDiagnostic> diagnostics() const override;
    std::unique_ptr<ISessionManager> releaseSessionManager() override;

private:
    void resolveTrust(const CodingStartupOptions& options);
    void registerTools();
    ResourceLoaderOptions resourceOptions(const CodingStartupOptions& options) const;
    ModelChoice chooseModel(const CodingStartupOptions& options);
    std::optional<std::vector<std::string>> activeTools(const CodingStartupOptions& options) const;
    void createSession(const CodingStartupOptions& options);
    void loadPlugins(const CodingStartupOptions& options);
    void startMcp(const CodingStartupOptions& options);
    void activateExtraTools(const std::vector<std::string>& names);
    std::vector<std::string> extraToolNames() const;
    std::optional<std::string> providerToken(const std::string& provider);
    void warn(const std::string& message);

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
    PluginDiscovery m_pluginDiscovery;
    PluginHost m_plugins;
    BashCommandExecutor m_bash;
    ModelSelector m_selector;
    ConfigValueResolver m_configValues;
    McpConfigLoader m_mcpConfigs;
    McpToolNamer m_mcpNamer;
    McpResultConverter m_mcpResults;
    McpConnector m_mcpConnector;
    std::unique_ptr<McpServerManager> m_mcp;
    std::unique_ptr<AgentSession> m_session;
};

CodingSessionHandle::CodingSessionHandle(SessionRuntimeRequest request, CodingServices& services,
                                         const CodingStartupOptions& options)
    : m_services(services),
      m_cwd(request.cwd),
      m_agentDir(request.agentDir),
      m_manager(std::move(request.sessionManager)),
      m_settings(m_agentDir + "/settings.json", m_cwd + "/.pi/settings.json", false,
                 services.platform().files(), services.platform().locks(), services.platform().ids()),
      m_resources(resourceOptions(options), m_settings, services.platform().files()),
      m_queue(services.platform().files()),
      m_pluginDiscovery(services.platform().files()),
      m_plugins(services.platform().libraries(), m_tools, m_hooks, services.platform().processes(),
                services.platform().logger(), PluginContext{request.cwd, request.agentDir}),
      m_bash(services.platform().processes(), services.platform().files(), services.platform().crypto(),
             services.platform().environment()),
      m_configValues(services.platform().environment(), services.platform().processes()),
      m_mcpConfigs(services.platform().files()),
      m_mcpNamer(services.platform().crypto()),
      m_mcpResults(services.platform().files(), services.platform().crypto(), services.platform().base64(),
                   services.platform().environment()),
      m_mcpConnector(services.platform().children(), services.platform().http(), services.platform().sleeper(),
                     services.platform().files(), m_configValues,
                     [this](const std::string& provider) { return providerToken(provider); }) {
    resolveTrust(options);
    registerTools();
    loadPlugins(options);
    startMcp(options);
    if (auto loaded = m_resources.reload(); !loaded) {
        warn(loaded.error().message);
    }
    createSession(options);
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

void CodingSessionHandle::warn(const std::string& message) {
    m_diagnostics.push_back(RuntimeDiagnostic{"warning", message});
}

CodingSessionHandle::~CodingSessionHandle() {
    m_hooks.emit("session_shutdown", Json{{"type", "session_shutdown"}});
    // The listeners and tools below use the session and the registry: stop them first.
    if (m_mcp) {
        m_mcp->close();
    }
    m_plugins.shutdown();
}

/** Loads plugins from <agent-dir>/plugins, the trusted project's .pi/plugins and --plugin. */
void CodingSessionHandle::loadPlugins(const CodingStartupOptions& options) {
    if (options.noPlugins) {
        return;
    }
    std::vector<std::string> paths = m_pluginDiscovery.discover(m_agentDir + "/plugins");
    if (m_settings.projectTrusted()) {
        for (std::string& path : m_pluginDiscovery.discover(m_cwd + "/.pi/plugins")) {
            paths.push_back(std::move(path));
        }
    }
    paths.insert(paths.end(), options.pluginPaths.begin(), options.pluginPaths.end());
    for (const Error& error : m_plugins.load(paths)) {
        warn("Plugin: " + error.message);
    }
}

std::optional<std::string> CodingSessionHandle::providerToken(const std::string& provider) {
    const auto auth = m_services.models().models().getAuth(provider, std::nullopt, {});
    if (!auth || !*auth) {
        return std::nullopt;
    }
    return (*auth)->auth.apiKey;
}

/** Connects the servers of mcp.json. Skipped when the tool set is restricted, since MCP tools are not in it. */
void CodingSessionHandle::startMcp(const CodingStartupOptions& options) {
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

/** Tools beyond the built-in set (from plugins and MCP servers). */
std::vector<std::string> CodingSessionHandle::extraToolNames() const {
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

void CodingSessionHandle::activateExtraTools(const std::vector<std::string>& names) {
    std::vector<std::string> active = m_session->activeToolNames();
    for (const std::string& name : names) {
        if (std::find(active.begin(), active.end(), name) == active.end()) {
            active.push_back(name);
        }
    }
    m_session->setActiveToolsByName(active);
}

void CodingSessionHandle::resolveTrust(const CodingStartupOptions& options) {
    const std::string policy = m_settings.view().defaultProjectTrust();
    const auto trusted = m_services.trust().resolve(m_cwd, options.trustProject, policy, {});
    if (!trusted) {
        warn("Could not read project trust: " + trusted.error().message);
        return;
    }
    if (*trusted) {
        m_settings.setProjectTrusted(true);
    }
}

void CodingSessionHandle::registerTools() {
    PlatformServices& platform = m_services.platform();
    const SettingsView view = m_settings.view();
    m_tools.add(std::make_shared<ReadTool>(platform.files(), platform.base64(), m_cwd));
    m_tools.add(std::make_shared<BashTool>(platform.processes(), platform.files(), platform.crypto(),
                                           platform.environment(), m_cwd, view.shellPath().value_or(""),
                                           view.shellCommandPrefix().value_or("")));
    m_tools.add(std::make_shared<EditTool>(platform.files(), m_queue, m_cwd));
    m_tools.add(std::make_shared<WriteTool>(platform.files(), m_queue, m_cwd));
    m_tools.add(std::make_shared<GrepTool>(platform.processes(), platform.files(), m_cwd));
    m_tools.add(std::make_shared<FindTool>(platform.files(), m_cwd));
    m_tools.add(std::make_shared<LsTool>(platform.files(), m_cwd));
}

ResourceLoaderOptions CodingSessionHandle::resourceOptions(const CodingStartupOptions& options) const {
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

ModelChoice CodingSessionHandle::chooseModel(const CodingStartupOptions& options) {
    IModelRuntime& models = m_services.models().models();
    const SessionContext context = m_manager->buildSessionContext();
    ModelSelectionInput input;
    input.available = models.availableModels();
    input.all = models.models();
    input.saved = context.model;
    input.savedThinking = context.model ? std::optional<std::string>(context.thinkingLevel) : std::nullopt;
    input.requestedModel = options.model;
    input.requestedThinking = options.thinking;
    return m_selector.select(input, m_settings.view());
}

std::optional<std::vector<std::string>> CodingSessionHandle::activeTools(const CodingStartupOptions& options) const {
    if (options.noTools) {
        return std::vector<std::string>{};
    }
    if (options.tools) {
        return options.tools;
    }
    return m_settings.view().defaultTools();
}

void CodingSessionHandle::createSession(const CodingStartupOptions& options) {
    PlatformServices& platform = m_services.platform();
    ModelChoice choice = chooseModel(options);
    for (const auto& message : choice.warnings) {
        warn(message);
    }
    AgentSessionConfig config{m_services.agents(),
                              *m_manager,
                              m_settings,
                              m_services.models().models(),
                              m_resources,
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
    m_session = std::make_unique<AgentSession>(std::move(config));
}

IAgentSession& CodingSessionHandle::session() {
    return *m_session;
}

ISessionManager& CodingSessionHandle::sessionManager() {
    return *m_manager;
}

std::string CodingSessionHandle::cwd() const {
    return m_cwd;
}

std::string CodingSessionHandle::agentDir() const {
    return m_agentDir;
}

std::vector<RuntimeDiagnostic> CodingSessionHandle::diagnostics() const {
    return m_diagnostics;
}

std::unique_ptr<ISessionManager> CodingSessionHandle::releaseSessionManager() {
    return std::move(m_manager);
}
