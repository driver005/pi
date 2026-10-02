export module pi.types.agent_session_config;

import std;
export import pi.agent.i_agent_factory;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.platform.i_sleeper;
export import pi.plugin.i_hook_bus;
export import pi.provider.i_model_runtime;
export import pi.session.i_resource_loader;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
export import pi.support.bash_command_executor;
export import pi.tool.i_tool_registry;
export import pi.types.model;
export import pi.types.scoped_model;
export import pi.types.thinking_level;

/**
 * Everything an AgentSession is built from. References must outlive the session; the caller
 * (the composition root) has already picked the starting model and thinking level.
 */
export struct AgentSessionConfig {
    IAgentFactory& agents;
    ISessionManager& session;
    ISettingsManager& settings;
    IModelRuntime& models;
    IResourceLoader& resources;
    IToolRegistry& tools;
    BashCommandExecutor& bash;
    IFileSystem& files;
    const IClock& clock;
    IIdGenerator& ids;
    ISleeper& sleeper;
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    std::string cwd;
    /** Tools active at start; nullopt restores the set the transcript declares. */
    std::optional<std::vector<std::string>> initialActiveTools;
    /** When set, only these tool names can ever be active. */
    std::optional<std::set<std::string>> allowedTools;
    std::set<std::string> excludedTools;
    std::vector<ScopedModel> scopedModels;
    /** Plugin hooks (tool_call, tool_result, context and agent events); null disables them. */
    IHookBus* hooks = nullptr;
};
