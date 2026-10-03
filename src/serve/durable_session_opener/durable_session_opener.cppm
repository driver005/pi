export module pi.serve.durable_session_opener;

import std;
export import pi.durable.i_storage;
export import pi.provider.i_model_runtime;
export import pi.server.i_session_opener;
export import pi.support.durable_models_service;
export import pi.types.harness_run_settings;
import pi.support.builtin_tasks;
import pi.support.directory_execution_env;
import pi.support.durable_served_session;
import pi.support.pi_prompt_extension;
import pi.support.registry;
import pi.support.resource_set_cache;
import pi.support.tool_bridge;
import pi.support.tool_set_cache;

/**
 * ISessionOpener that starts the durable session of a cataloged session: its storage is `session.sqlite` in the session's
 * directory (the layout of the TS server), over which a harness runs with pi's coding tools and system prompt installed
 * in a registry of its own. The root conversation is created on first open with the agent seed (directory, model, thinking
 * level) and found again after that; tasks a stop interrupted resume. Counterpart of createSessionWorkerServices.
 */
export class DurableSessionOpener : public ISessionOpener {
public:
    using StorageOpener = std::function<Result<std::shared_ptr<IStorage>>(const std::string& path)>;
    using SettingsSource = std::function<HarnessRunSettings(const std::string& cwd)>;
    /** The `pi.agent` change a new root conversation starts with. */
    using AgentSeed = std::function<Json(const std::string& cwd)>;

    DurableSessionOpener(IModelRuntime& models, StorageOpener storage, std::shared_ptr<ToolSetCache> tools, std::shared_ptr<ResourceSetCache> resources, SettingsSource settings, AgentSeed seed,
                         DurableModelsService::SelectedListener onSelected)
        : m_models(models),
          m_storage(std::move(storage)),
          m_tools(std::move(tools)),
          m_resources(std::move(resources)),
          m_settings(std::move(settings)),
          m_seed(std::move(seed)),
          m_onSelected(std::move(onSelected)) {}

    Result<std::shared_ptr<IRoutedSessionHandle>> open(const SessionRecord& record, const ServiceContext&) override {
        auto storage = m_storage(record.directory + "/session.sqlite");
        if (!storage) {
            return std::unexpected(storage.error());
        }
        auto registry = std::make_shared<Registry>(BuiltinTasks().all());
        if (auto installed = registry->install(ToolBridge(m_tools, record.cwd).extension("coding-tools")); !installed) {
            return std::unexpected(installed.error());
        }
        if (auto installed = registry->install(PiPromptExtension(m_tools, m_resources, record.cwd).extension()); !installed) {
            return std::unexpected(installed.error());
        }
        HarnessOptions options;
        options.models = &m_models;
        options.registry = registry.get();
        options.settings = [settings = m_settings, cwd = record.cwd] { return settings(cwd); };
        options.env = [cwd = record.cwd](std::int64_t, const std::optional<std::string>& conversationCwd) -> Result<std::shared_ptr<IExecutionEnv>> {
            return std::shared_ptr<IExecutionEnv>(std::make_shared<DirectoryExecutionEnv>(conversationCwd.value_or(cwd)));
        };
        auto harness = std::make_unique<Harness>(*storage, options);
        if (auto opened = harness->open(); !opened) {
            return std::unexpected(opened.error());
        }
        ConversationCreateOptions create;
        create.agent = m_seed(record.cwd);
        auto root = harness->root(create);
        if (!root) {
            return std::unexpected(root.error());
        }
        if (auto resumed = harness->resume(); !resumed) {
            return std::unexpected(resumed.error());
        }
        // Tools of plugins and MCP servers can appear while the session runs: the extension is installed again then.
        const std::int64_t subscription = m_tools->subscribe([weak = std::weak_ptr<Registry>(registry), tools = m_tools, cwd = record.cwd](const std::string& changed) {
            if (const std::shared_ptr<Registry> live = weak.lock(); live && changed == cwd) {
                (void)live->install(ToolBridge(tools, cwd).extension("coding-tools"));
            }
        });
        return std::shared_ptr<IRoutedSessionHandle>(std::make_shared<DurableServedSession>(registry, std::move(harness), *root, m_models, m_onSelected, [tools = m_tools, subscription] { tools->unsubscribe(subscription); }));
    }

private:
    IModelRuntime& m_models;
    StorageOpener m_storage;
    std::shared_ptr<ToolSetCache> m_tools;
    std::shared_ptr<ResourceSetCache> m_resources;
    SettingsSource m_settings;
    AgentSeed m_seed;
    DurableModelsService::SelectedListener m_onSelected;
};
