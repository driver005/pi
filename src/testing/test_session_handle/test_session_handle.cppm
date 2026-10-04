export module pi.testing.test_session_handle;

import std;
export import pi.agent.i_agent_factory;
export import pi.platform.i_clock;
export import pi.platform.i_file_system;
export import pi.platform.i_id_generator;
export import pi.platform.i_sleeper;
export import pi.session.i_session_runtime_handle;
export import pi.types.session_runtime_request;
import pi.ai.faux_provider;
import pi.base.boring_crypto;
import pi.base.posix_process_runner;
import pi.base.system_environment;
import pi.session.agent_session;
import pi.testing.fake_model_runtime;
import pi.testing.fake_resource_loader;
import pi.testing.fake_settings_manager;
import pi.tools.tool_registry;

/** A real AgentSession over a faux provider, with fake settings and resources, for runtime tests. `denyReplacement` (read at each question) makes it refuse to be switched or forked, like a plugin that cancels. */
export class TestSessionHandle : public ISessionRuntimeHandle {
public:
    TestSessionHandle(SessionRuntimeRequest request, IAgentFactory& agents, FauxProvider& provider, IFileSystem& files, const IClock& clock, IIdGenerator& ids, ISleeper& sleeper, const bool* denyReplacement = nullptr)
        : m_denyReplacement(denyReplacement),
          m_cwd(request.cwd),
          m_agentDir(request.agentDir),
          m_manager(std::move(request.sessionManager)),
          m_bash(m_runner, files, m_crypto, m_environment) {
        m_models.addModel(fauxModel());
        m_models.setAuthenticated("faux", true);
        m_models.setStreamHandler([&provider](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            return provider.stream(model, context, options);
        });
        AgentSessionConfig config{agents, *m_manager, m_settings, m_models, m_resources, m_tools, m_bash, files,
                                  clock,  ids,        sleeper,    fauxModel(), ThinkingLevel::Off, m_cwd,
                                  std::nullopt, std::nullopt, {}, {}};
        m_session = std::make_unique<AgentSession>(config);
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
        return {};
    }

    bool allowSwitch(const std::string&, const std::optional<std::string>&) override {
        return m_denyReplacement == nullptr || !*m_denyReplacement;
    }

    bool allowFork(const std::string&, ForkPosition) override {
        return m_denyReplacement == nullptr || !*m_denyReplacement;
    }

    std::unique_ptr<ISessionManager> releaseSessionManager() override {
        return std::move(m_manager);
    }

private:
    Model fauxModel() const {
        Model model;
        model.id = "faux-1";
        model.provider = "faux";
        model.api = "faux";
        model.contextWindow = 100000;
        model.maxTokens = 8000;
        return model;
    }

    const bool* m_denyReplacement;
    std::string m_cwd;
    std::string m_agentDir;
    std::unique_ptr<ISessionManager> m_manager;
    FakeSettingsManager m_settings;
    FakeModelRuntime m_models;
    FakeResourceLoader m_resources;
    ToolRegistry m_tools;
    PosixProcessRunner m_runner;
    BoringCrypto m_crypto;
    SystemEnvironment m_environment;
    BashCommandExecutor m_bash;
    std::unique_ptr<AgentSession> m_session;
};
