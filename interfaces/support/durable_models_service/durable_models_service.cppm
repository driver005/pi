export module pi.support.durable_models_service;

import std;
export import pi.chord.i_remote_service;
export import pi.provider.i_model_runtime;
export import pi.support.conversation;
export import pi.support.replicated_state;
export import pi.support.thinking_level_resolver;

/**
 * The `pi.models` service over a durable conversation: the models it may use, its model and thinking level as replicated
 * state and the calls that change them. The configuration follows the conversation's `pi.agent` document, as seen in its
 * view state, so changes made by any client are published. State: `{catalog: {revision, availableModels},
 * configuration: {model, thinkingLevel}, refresh: {status}}`. Port of models-provider.ts. `onSelected` runs after a model
 * selection took effect, so the host can save it as the user's default.
 */
export class DurableModelsService : public IRemoteService {
public:
    using SelectedListener = std::function<void(const Model&)>;

    DurableModelsService(std::shared_ptr<Conversation> conversation, std::shared_ptr<IReplicatedState> view, IModelRuntime& models, SelectedListener onSelected)
        : m_conversation(std::move(conversation)),
          m_view(std::move(view)),
          m_models(models),
          m_onSelected(std::move(onSelected)),
          m_state(Json{{"catalog", catalog(1)}, {"configuration", configuration()}, {"refresh", Json{{"status", "idle"}}}}) {
        m_listener = m_view->subscribe([this](const Json&, std::int64_t, const ServiceContext&) { publishConfiguration(); });
        publishConfiguration();
    }

    ~DurableModelsService() override {
        m_view->unsubscribe(m_listener);
    }

    DurableModelsService(const DurableModelsService&) = delete;
    DurableModelsService& operator=(const DurableModelsService&) = delete;

    std::map<std::string, Method> methods() override {
        std::map<std::string, Method> methods;
        methods["cycleThinking"] = [this](const std::vector<Json>&, const ServiceContext&) { return cycleThinking(); };
        methods["getThinkingLevels"] = [this](const std::vector<Json>&, const ServiceContext&) { return getThinkingLevels(); };
        methods["refresh"] = [this](const std::vector<Json>&, const ServiceContext&) { return refresh(); };
        methods["select"] = [this](const std::vector<Json>& args, const ServiceContext&) { return select(args); };
        methods["selectThinking"] = [this](const std::vector<Json>& args, const ServiceContext&) { return selectThinking(args); };
        return methods;
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {{"state", &m_state}};
    }

private:
    Result<std::optional<Json>> cycleThinking() {
        const std::vector<std::string> levels = levelNames();
        const std::string current = agentDocument().value("thinkingLevel", std::string("off"));
        const auto at = std::ranges::find(levels, current);
        const std::size_t next = at == levels.end() ? 0 : (static_cast<std::size_t>(at - levels.begin()) + 1) % levels.size();
        if (auto configured = m_conversation->configure(Json{{"thinkingLevel", levels[next]}}); !configured) {
            return std::unexpected(configured.error());
        }
        return std::optional<Json>();
    }

    Result<std::optional<Json>> getThinkingLevels() {
        Json levels = Json::array();
        for (const std::string& name : levelNames()) {
            levels.push_back(name);
        }
        return std::optional<Json>(std::move(levels));
    }

    Result<std::optional<Json>> refresh() {
        setRefresh(Json{{"status", "refreshing"}});
        const auto reloaded = m_models.reload();
        const std::optional<std::string> problem = m_models.error();
        const Json next = catalog(++m_revision);
        m_state.change(background(), [&](Json& draft) {
            draft["catalog"] = next;
            if (!reloaded || problem) {
                draft["refresh"] = Json{{"status", "warning"}, {"errors", Json{{"models.json", problem ? *problem : reloaded.error().message}}}};
            } else {
                draft["refresh"] = Json{{"status", "done"}};
            }
        });
        return std::optional<Json>();
    }

    Result<std::optional<Json>> select(const std::vector<Json>& args) {
        if (args.size() != 1 || !args[0].is_object() || !args[0].value("provider", Json()).is_string() || !args[0].value("modelId", Json()).is_string()) {
            return std::unexpected(Error{"invalid_request", "Invalid model selection"});
        }
        const std::string provider = args[0].at("provider").get<std::string>();
        const std::string id = args[0].at("modelId").get<std::string>();
        const auto model = m_models.find(provider, id);
        if (!model) {
            return std::unexpected(Error{"model_not_found", "Unknown model " + provider + "/" + id});
        }
        const auto requested = m_levels.parseLevel(agentDocument().value("thinkingLevel", std::string("off")));
        const std::string level = m_levels.levelName(m_levels.clamp(*model, requested.value_or(ThinkingLevel::Off)));
        if (auto configured = m_conversation->configure(Json{{"model", Json{{"provider", model->provider}, {"modelId", model->id}}}, {"thinkingLevel", level}}); !configured) {
            return std::unexpected(configured.error());
        }
        if (m_onSelected) {
            m_onSelected(*model);
        }
        return std::optional<Json>();
    }

    Result<std::optional<Json>> selectThinking(const std::vector<Json>& args) {
        if (args.size() != 1 || !args[0].is_string()) {
            return std::unexpected(Error{"invalid_request", "Invalid thinking level"});
        }
        const std::string level = args[0].get<std::string>();
        const std::vector<std::string> levels = levelNames();
        if (std::ranges::find(levels, level) == levels.end()) {
            std::string choices;
            for (const std::string& name : levels) {
                choices += (choices.empty() ? "" : ", ") + name;
            }
            return std::unexpected(Error{"invalid_request", "Thinking level " + level + " is unavailable; choose one of: " + choices});
        }
        if (auto configured = m_conversation->configure(Json{{"thinkingLevel", level}}); !configured) {
            return std::unexpected(configured.error());
        }
        return std::optional<Json>();
    }

    /** The conversation's `pi.agent` document as the view shows it; an empty object while it has none. */
    Json agentDocument() const {
        const Json value = m_view->snapshot().value;
        return value.value("docs", Json::object()).value("pi.agent", Json::object());
    }

    std::optional<Model> selectedModel() const {
        const Json agent = agentDocument();
        if (!agent.contains("model")) {
            return std::nullopt;
        }
        return m_models.find(agent.at("model").value("provider", std::string()), agent.at("model").value("modelId", std::string()));
    }

    std::vector<std::string> levelNames() const {
        const std::optional<Model> selected = selectedModel();
        std::vector<std::string> names;
        if (!selected) {
            return {"off"};
        }
        for (const ThinkingLevel level : m_levels.supportedLevels(*selected)) {
            names.push_back(m_levels.levelName(level));
        }
        return names;
    }

    Json catalog(std::int64_t revision) const {
        std::vector<Model> shown = m_models.availableModels();
        if (const std::optional<Model> selected = selectedModel();
            selected && std::ranges::none_of(shown, [&](const Model& model) { return model.provider == selected->provider && model.id == selected->id; })) {
            shown.push_back(*selected);
        }
        Json available = Json::array();
        for (const Model& model : shown) {
            available.push_back(Json{{"provider", model.provider}, {"modelId", model.id}, {"name", model.name.empty() ? model.id : model.name}, {"reasoning", model.reasoning}});
        }
        return Json{{"revision", revision}, {"availableModels", std::move(available)}};
    }

    Json configuration() const {
        const Json agent = agentDocument();
        Json model = nullptr;
        if (agent.contains("model")) {
            model = Json{{"provider", agent.at("model").value("provider", std::string())}, {"modelId", agent.at("model").value("modelId", std::string())}};
        }
        return Json{{"model", std::move(model)}, {"thinkingLevel", agent.value("thinkingLevel", std::string("off"))}};
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

    std::shared_ptr<Conversation> m_conversation;
    std::shared_ptr<IReplicatedState> m_view;
    IModelRuntime& m_models;
    SelectedListener m_onSelected;
    ThinkingLevelResolver m_levels;
    std::int64_t m_revision = 1;
    ReplicatedState m_state;
    std::uint64_t m_listener = 0;
};
