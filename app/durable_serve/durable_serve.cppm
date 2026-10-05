export module pi.durable_serve;

import std;
export import pi.coding_services;
export import pi.server.i_session_opener;
export import pi.types.coding_startup_options;
import pi.durable_toolbox;
import pi.plugin.i_plugin_commands;
import pi.durable.sqlite_database;
import pi.durable.sqlite_storage;
import pi.serve.durable_session_opener;
import pi.session.resource_loader;
import pi.session.settings_manager;
import pi.support.model_selector;
import pi.support.package_manager;
import pi.support.resource_set_cache;
import pi.support.tool_set_cache;
import pi.tools.bash_tool;
import pi.tools.edit_tool;
import pi.tools.file_mutation_queue;
import pi.tools.find_tool;
import pi.tools.grep_tool;
import pi.tools.ls_tool;
import pi.tools.read_tool;
import pi.tools.write_tool;

/**
 * The composition of durable sessions for `pi serve`: SQLite session files, pi's coding tools and resources per working
 * directory, the user's settings as harness policy, and the model a new session starts with. Everything per directory
 * (settings, tools, resources) is built on first use and kept, like pi at startup. The tools of plugins and MCP servers
 * (`DurableToolbox`) are offered next to the built-in ones; a server that connects late drops the directory's tool set, which
 * tells the sessions to install their tools again. The hooks of the plugins reach the sessions of their directory through the
 * `plugin-hooks` extension, and a session's `pi.session-plugins` service reloads the plugins of its directory.
 */
export class DurableServe {
public:
    DurableServe(CodingServices& services, CodingStartupOptions startup, std::string agentDir)
        : m_services(services),
          m_startup(std::move(startup)),
          m_agentDir(std::move(agentDir)),
          m_queue(services.platform().files()),
          m_tools(std::make_shared<ToolSetCache>([this](const std::string& cwd) { return buildTools(cwd); })),
          m_resources(std::make_shared<ResourceSetCache>([this](const std::string& cwd) { return loadResources(cwd); })) {}

    DurableServe(const DurableServe&) = delete;
    DurableServe& operator=(const DurableServe&) = delete;

    std::shared_ptr<ISessionOpener> opener() {
        return std::make_shared<DurableSessionOpener>(
            m_services.models().models(), [this](const std::string& path) { return openStorage(path); }, m_tools, m_resources,
            [this](const std::string& cwd) { return runSettings(cwd); }, [this](const std::string& cwd) { return seed(cwd); },
            [this](const Model& model) { saveDefault(model); }, [this](const std::string& cwd) { return toolboxFor(cwd).hooks(); }, [this](const std::string& cwd) { return toolboxFor(cwd).reload(); },
            [this](const std::string& cwd) -> IPluginCommands* { return &toolboxFor(cwd).commands(); });
    }

private:
    Result<std::shared_ptr<IStorage>> openStorage(const std::string& path) {
        auto database = std::make_shared<SqliteDatabase>(path);
        if (auto opened = database->open(); !opened) {
            return std::unexpected(opened.error());
        }
        auto storage = std::make_shared<SqliteStorage>(database);
        if (auto opened = storage->open(); !opened) {
            return std::unexpected(opened.error());
        }
        return std::shared_ptr<IStorage>(storage);
    }

    /** The settings of one directory; project settings count once the project is trusted. */
    std::shared_ptr<SettingsManager> settingsFor(const std::string& cwd) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_settings.find(cwd);
        if (found != m_settings.end()) {
            return found->second;
        }
        PlatformServices& platform = m_services.platform();
        auto settings = std::make_shared<SettingsManager>(m_agentDir + "/settings.json", cwd + "/.pi/settings.json", false, platform.files(), platform.locks(), platform.ids());
        const auto trusted = m_services.trust().resolve(cwd, m_startup.trustProject, settings->view().defaultProjectTrust(), {});
        if (trusted && *trusted) {
            settings->setProjectTrusted(true);
        }
        m_settings.emplace(cwd, settings);
        return settings;
    }

    ToolSetCache::ToolSet buildTools(const std::string& cwd) {
        PlatformServices& platform = m_services.platform();
        const SettingsView view = settingsFor(cwd)->view();
        ToolSetCache::ToolSet tools;
        tools.push_back(std::make_shared<ReadTool>(platform.files(), platform.base64(), cwd));
        tools.push_back(std::make_shared<BashTool>(platform.processes(), platform.files(), platform.crypto(), platform.environment(), cwd, view.shellPath().value_or(""), view.shellCommandPrefix().value_or("")));
        tools.push_back(std::make_shared<EditTool>(platform.files(), m_queue, cwd));
        tools.push_back(std::make_shared<WriteTool>(platform.files(), m_queue, cwd));
        tools.push_back(std::make_shared<GrepTool>(platform.processes(), platform.files(), cwd));
        tools.push_back(std::make_shared<FindTool>(platform.files(), cwd));
        tools.push_back(std::make_shared<LsTool>(platform.files(), cwd));
        for (const std::shared_ptr<ITool>& tool : toolboxFor(cwd).tools()) {
            tools.push_back(tool);
        }
        const std::optional<std::vector<std::string>> allowed = m_startup.noTools ? std::optional<std::vector<std::string>>(std::vector<std::string>{}) : m_startup.tools ? m_startup.tools : settingsFor(cwd)->view().defaultTools();
        if (allowed) {
            std::erase_if(tools, [&](const std::shared_ptr<ITool>& tool) { return std::ranges::find(*allowed, tool->definition().name) == allowed->end(); });
        }
        return tools;
    }

    /** The toolbox of one directory, made once; tools a server adds later drop the directory's tool set. */
    DurableToolbox& toolboxFor(const std::string& cwd) {
        const std::lock_guard<std::mutex> lock(m_toolboxMutex);
        auto found = m_toolboxes.find(cwd);
        if (found == m_toolboxes.end()) {
            auto toolbox = std::make_unique<DurableToolbox>(m_services, *settingsFor(cwd), cwd, m_agentDir, m_startup);
            toolbox->onChange([this, cwd] { m_tools->invalidate(cwd); });
            found = m_toolboxes.emplace(cwd, std::move(toolbox)).first;
        }
        return *found->second;
    }

    LoadedResources loadResources(const std::string& cwd) {
        ResourceLoaderOptions options;
        options.cwd = cwd;
        options.agentDir = m_agentDir;
        options.systemPrompt = m_startup.systemPrompt;
        options.appendSystemPrompt = m_startup.appendSystemPrompt;
        options.noContextFiles = m_startup.noContextFiles;
        options.noSkills = m_startup.noSkills;
        options.noPromptTemplates = m_startup.noPromptTemplates;
        options.additionalSkillPaths = m_startup.skillPaths;
        options.additionalPromptTemplatePaths = m_startup.promptTemplatePaths;
        const std::shared_ptr<SettingsManager> settings = settingsFor(cwd);
        ResourceLoader loader(std::move(options), *settings, m_services.platform().files());
        PlatformServices& platform = m_services.platform();
        PackageManager packages(platform.files(), platform.processes(), *settings, cwd, m_agentDir);
        const PackagePaths found = packages.load(true);
        loader.addPaths(m_startup.noSkills ? std::vector<std::string>{} : found.skills, m_startup.noPromptTemplates ? std::vector<std::string>{} : found.prompts);
        (void)loader.reload();
        return loader.resources();
    }

    HarnessRunSettings runSettings(const std::string& cwd) {
        const SettingsView view = settingsFor(cwd)->view();
        const AssistantRetryPolicy retry = view.retryPolicy();
        const CompactionSettings compaction = view.compactionSettings("", "");
        HarnessRunSettings settings;
        settings.retry = Json{{"enabled", retry.enabled}, {"maxRetries", retry.maxRetries}, {"baseDelayMs", retry.baseDelayMs}, {"maxAgentDelayMs", retry.maxDelayMs}};
        settings.compaction = Json{{"enabled", compaction.enabled}, {"reserveTokens", compaction.reserveTokens}, {"keepRecentTokens", compaction.keepRecentTokens}};
        settings.steeringMode = view.steeringMode();
        settings.followUpMode = view.followUpMode();
        return settings;
    }

    /** The `pi.agent` a new root conversation starts with: the directory and the startup (or default) model. */
    Json seed(const std::string& cwd) {
        IModelRuntime& models = m_services.models().models();
        ModelSelectionInput input;
        input.available = models.availableModels();
        input.all = models.models();
        input.requestedModel = m_startup.model;
        input.requestedThinking = m_startup.thinking;
        const ModelChoice choice = ModelSelector().select(input, settingsFor(cwd)->view());
        Json agent = Json::object({{"cwd", cwd}});
        if (choice.model) {
            agent["model"] = Json{{"provider", choice.model->provider}, {"modelId", choice.model->id}};
            agent["thinkingLevel"] = ThinkingLevelResolver().levelName(choice.thinkingLevel);
        }
        return agent;
    }

    /** A model chosen by a client becomes the user's default, as in the TS service. */
    void saveDefault(const Model& model) {
        const std::shared_ptr<SettingsManager> settings = settingsFor(m_agentDir);
        (void)settings->setGlobal("defaultProvider", model.provider);
        (void)settings->setGlobal("defaultModel", model.id);
    }

    CodingServices& m_services;
    CodingStartupOptions m_startup;
    std::string m_agentDir;
    FileMutationQueue m_queue;
    std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<SettingsManager>> m_settings;
    std::shared_ptr<ToolSetCache> m_tools;
    std::shared_ptr<ResourceSetCache> m_resources;
    std::mutex m_toolboxMutex;
    // After the settings the toolboxes use; destroyed first.
    std::map<std::string, std::unique_ptr<DurableToolbox>> m_toolboxes;
};
