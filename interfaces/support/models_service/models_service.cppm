export module pi.support.models_service;

import std;
export import pi.chord.i_remote_service;
export import pi.provider.i_model_runtime;
export import pi.session.i_agent_session;
export import pi.support.message_codec;
export import pi.support.replicated_state;

/**
 * The `pi.models` service: the models a session may use, its model and thinking level as replicated
 * state, and the calls that change them. Selections are saved as the user's defaults, as in the TS
 * service. State: `{catalog: {revision, availableModels}, configuration: {model, thinkingLevel},
 * refresh: {status}}`. Port of models-provider.ts.
 */
export class ModelsService : public IRemoteService {
public:
    ModelsService(IAgentSession& session, IModelRuntime& models)
        : m_session(session),
          m_models(models),
          m_state(Json{{"catalog", catalog(1)}, {"configuration", configuration()}, {"refresh", Json{{"status", "idle"}}}}) {
        m_listener = m_session.subscribe([this](const AgentSessionEvent& event) {
            if (event.type == SessionEventType::ThinkingLevelChanged) {
                publishConfiguration();
            }
        });
    }

    ~ModelsService() override {
        m_session.unsubscribe(m_listener);
    }

    ModelsService(const ModelsService&) = delete;
    ModelsService& operator=(const ModelsService&) = delete;

    std::map<std::string, Method> methods() override {
        std::map<std::string, Method> methods;
        methods["cycleThinking"] = [this](const std::vector<Json>&, const ServiceContext&) { return cycleThinking(); };
        methods["getThinkingLevels"] = [this](const std::vector<Json>&, const ServiceContext&) {
            return getThinkingLevels();
        };
        methods["refresh"] = [this](const std::vector<Json>&, const ServiceContext&) { return refresh(); };
        methods["select"] = [this](const std::vector<Json>& args, const ServiceContext&) { return select(args); };
        methods["selectThinking"] = [this](const std::vector<Json>& args, const ServiceContext&) {
            return selectThinking(args);
        };
        return methods;
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {{"state", &m_state}};
    }

private:
    Result<std::optional<Json>> cycleThinking() {
        m_session.cycleThinkingLevel(true);
        publishConfiguration();
        return std::optional<Json>();
    }

    Result<std::optional<Json>> getThinkingLevels() const {
        Json levels = Json::array();
        for (const ThinkingLevel level : m_session.availableThinkingLevels()) {
            levels.push_back(m_codec.thinkingLevelName(level));
        }
        return std::optional<Json>(std::move(levels));
    }

    Result<std::optional<Json>> refresh() {
        setRefresh(Json{{"status", "refreshing"}});
        const auto reloaded = m_models.reload();
        const std::optional<std::string> problem = m_models.error();
        const std::int64_t revision = ++m_revision;
        const Json next = catalog(revision);
        m_state.change(background(), [&](Json& draft) {
            draft["catalog"] = next;
            if (!reloaded || problem) {
                draft["refresh"] = Json{{"status", "warning"},
                                        {"errors", Json{{"models.json", problem ? *problem : reloaded.error().message}}}};
            } else {
                draft["refresh"] = Json{{"status", "done"}};
            }
        });
        return std::optional<Json>();
    }

    Result<std::optional<Json>> select(const std::vector<Json>& args) {
        if (args.size() != 1 || !args[0].is_object() || !args[0].value("provider", Json()).is_string() ||
            !args[0].value("modelId", Json()).is_string()) {
            return std::unexpected(Error{"invalid_request", "Invalid model selection"});
        }
        const std::string provider = args[0]["provider"].get<std::string>();
        const std::string id = args[0]["modelId"].get<std::string>();
        const auto model = m_models.find(provider, id);
        if (!model) {
            return std::unexpected(Error{"model_not_found", "Unknown model " + provider + "/" + id});
        }
        if (auto selected = m_session.setModel(*model, true); !selected) {
            return std::unexpected(selected.error());
        }
        publishConfiguration();
        return std::optional<Json>();
    }

    Result<std::optional<Json>> selectThinking(const std::vector<Json>& args) {
        if (args.size() != 1 || !args[0].is_string()) {
            return std::unexpected(Error{"invalid_request", "Invalid thinking level"});
        }
        const auto level = m_codec.parseThinkingLevel(args[0].get<std::string>());
        if (!level) {
            return std::unexpected(Error{"invalid_request", "Unknown thinking level " + args[0].get<std::string>()});
        }
        m_session.setThinkingLevel(*level, true);
        publishConfiguration();
        return std::optional<Json>();
    }

    Json catalog(std::int64_t revision) const {
        Json available = Json::array();
        for (const Model& model : m_models.availableModels()) {
            available.push_back(Json{{"provider", model.provider},
                                     {"modelId", model.id},
                                     {"name", model.name.empty() ? model.id : model.name},
                                     {"reasoning", model.reasoning}});
        }
        return Json{{"revision", revision}, {"availableModels", std::move(available)}};
    }

    Json configuration() const {
        const Model model = m_session.model();
        Json selected = nullptr;
        if (!model.id.empty()) {
            selected = Json{{"provider", model.provider}, {"modelId", model.id}};
        }
        return Json{{"model", std::move(selected)}, {"thinkingLevel", m_codec.thinkingLevelName(m_session.thinkingLevel())}};
    }

    void publishConfiguration() {
        const Json next = configuration();
        m_state.change(background(), [&](Json& draft) { draft["configuration"] = next; });
    }

    void setRefresh(const Json& refresh) {
        m_state.change(background(), [&](Json& draft) { draft["refresh"] = refresh; });
    }

    ServiceContext background() const {
        return ServiceContext{std::make_shared<AbortSignal>()};
    }

    IAgentSession& m_session;
    IModelRuntime& m_models;
    MessageCodec m_codec;
    std::int64_t m_revision = 1;
    ReplicatedState m_state;
    IAgentSession::ListenerId m_listener = 0;
};
