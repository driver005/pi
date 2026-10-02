module;

#include <nlohmann/json.hpp>

export module pi.session.model_controller;

import std;
export import pi.agent.i_agent;
export import pi.provider.i_model_runtime;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
export import pi.support.thinking_level_resolver;
export import pi.types.model_cycle_result;
export import pi.types.scoped_model;

/**
 * Selects the session's model and thinking level: direct selection, cycling through the scoped or
 * available models, clamping the level to what the model supports, recording changes in the
 * session and optionally saving them as defaults. Port of the model and thinking sections of
 * AgentSession.
 */
export class ModelController {
public:
    ModelController(IAgent& agent, ISessionManager& session, ISettingsManager& settings, IModelRuntime& models,
                    ISessionEventSink& sink);

    /** Error when the provider has no credentials. persist saves the model as the global default. */
    Result<void> setModel(const Model& model, bool persist);

    /** nullopt when there is nothing to cycle to. */
    std::optional<ModelCycleResult> cycleModel(bool forward, bool persist);

    /** Clamps to the model's capabilities; records and announces the change only when it differs. */
    void setThinkingLevel(ThinkingLevel level, bool persist);
    std::optional<ThinkingLevel> cycleThinkingLevel(bool persist);
    std::vector<ThinkingLevel> availableThinkingLevels() const;
    bool supportsThinking() const;

    void setScopedModels(std::vector<ScopedModel> scoped);
    std::vector<ScopedModel> scopedModels() const;

private:
    bool sameModel(const Model& left, const Model& right) const;
    ThinkingLevel levelForSwitch(const Model& target, const std::optional<ThinkingLevel>& explicitLevel) const;
    void apply(const Model& model, const std::optional<ThinkingLevel>& explicitLevel, bool persist);
    void addToScope(const Model& model);
    std::optional<ModelCycleResult> cycleScoped(bool forward, bool persist);
    std::optional<ModelCycleResult> cycleAvailable(bool forward, bool persist);
    std::size_t nextIndex(std::size_t current, std::size_t size, bool forward) const;
    std::string lower(std::string text) const;

    IAgent& m_agent;
    ISessionManager& m_session;
    ISettingsManager& m_settings;
    IModelRuntime& m_models;
    ISessionEventSink& m_sink;
    ThinkingLevelResolver m_levels;

    mutable std::mutex m_mutex;
    std::vector<ScopedModel> m_scoped;
};

ModelController::ModelController(IAgent& agent, ISessionManager& session, ISettingsManager& settings,
                                 IModelRuntime& models, ISessionEventSink& sink)
    : m_agent(agent), m_session(session), m_settings(settings), m_models(models), m_sink(sink) {}

bool ModelController::sameModel(const Model& left, const Model& right) const {
    return left.provider == right.provider && left.id == right.id;
}

std::string ModelController::lower(std::string text) const {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void ModelController::setScopedModels(std::vector<ScopedModel> scoped) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_scoped = std::move(scoped);
}

std::vector<ScopedModel> ModelController::scopedModels() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_scoped;
}

std::vector<ThinkingLevel> ModelController::availableThinkingLevels() const {
    const Model model = m_agent.model();
    if (model.id.empty()) {
        return {ThinkingLevel::Off, ThinkingLevel::Minimal, ThinkingLevel::Low,
                ThinkingLevel::Medium, ThinkingLevel::High};
    }
    return m_levels.supportedLevels(model);
}

bool ModelController::supportsThinking() const {
    return m_agent.model().reasoning;
}

ThinkingLevel ModelController::levelForSwitch(const Model& target,
                                              const std::optional<ThinkingLevel>& explicitLevel) const {
    if (explicitLevel) {
        return *explicitLevel;
    }
    const SettingsView view = m_settings.view();
    if (const auto perModel = view.modelThinkingLevel(target.provider, target.id)) {
        if (const auto parsed = m_levels.parseLevel(*perModel)) {
            return *parsed;
        }
    }
    if (const auto fallback = view.defaultThinkingLevel()) {
        if (const auto parsed = m_levels.parseLevel(*fallback)) {
            return *parsed;
        }
    }
    return m_agent.thinkingLevel();
}

void ModelController::setThinkingLevel(ThinkingLevel level, bool persist) {
    const Model model = m_agent.model();
    const auto available = availableThinkingLevels();
    const bool supported = std::ranges::find(available, level) != available.end();
    const ThinkingLevel effective =
        supported ? level : (model.id.empty() ? ThinkingLevel::Off : m_levels.clamp(model, level));
    const ThinkingLevel previous = m_agent.thinkingLevel();
    m_agent.setThinkingLevel(effective);
    if (persist) {
        m_settings.setGlobal("defaultThinkingLevel", m_levels.levelName(level));
    }
    if (effective != previous) {
        m_session.appendThinkingLevelChange(m_levels.levelName(effective));
        AgentSessionEvent event;
        event.type = SessionEventType::ThinkingLevelChanged;
        event.level = effective;
        m_sink.emit(event);
    }
}

std::optional<ThinkingLevel> ModelController::cycleThinkingLevel(bool persist) {
    if (!supportsThinking()) {
        return std::nullopt;
    }
    const auto levels = availableThinkingLevels();
    const auto current = std::ranges::find(levels, m_agent.thinkingLevel());
    const std::size_t index = current == levels.end() ? 0 : static_cast<std::size_t>(current - levels.begin());
    const ThinkingLevel next = levels[(index + 1) % levels.size()];
    setThinkingLevel(next, persist);
    return next;
}

void ModelController::addToScope(const Model& model) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_scoped.empty() ||
            std::ranges::any_of(m_scoped, [&](const ScopedModel& scoped) { return sameModel(scoped.model, model); })) {
            return;
        }
        m_scoped.push_back(ScopedModel{model, std::nullopt});
    }
    const auto enabled = m_settings.view().enabledModels();
    if (!enabled || enabled->empty()) {
        return;
    }
    const std::string reference = model.provider + "/" + model.id;
    if (std::ranges::any_of(*enabled, [&](const std::string& pattern) { return lower(pattern) == lower(reference); })) {
        return;
    }
    std::vector<std::string> next = *enabled;
    next.push_back(reference);
    m_settings.setGlobal("enabledModels", Json(next));
}

void ModelController::apply(const Model& model, const std::optional<ThinkingLevel>& explicitLevel, bool persist) {
    const ThinkingLevel level = levelForSwitch(model, explicitLevel);
    m_agent.setModel(model);
    m_session.appendModelChange(model.provider, model.id);
    if (persist) {
        m_settings.setGlobal("defaultProvider", model.provider);
        m_settings.setGlobal("defaultModel", model.id);
        addToScope(model);
    }
    // Model persistence does not rewrite the global thinking default.
    setThinkingLevel(level, false);
}

Result<void> ModelController::setModel(const Model& model, bool persist) {
    if (!m_models.hasConfiguredAuth(model.provider)) {
        return std::unexpected(Error{"no_auth", "No API key for " + model.provider + "/" + model.id});
    }
    apply(model, std::nullopt, persist);
    return {};
}

std::size_t ModelController::nextIndex(std::size_t current, std::size_t size, bool forward) const {
    return forward ? (current + 1) % size : (current + size - 1) % size;
}

std::optional<ModelCycleResult> ModelController::cycleScoped(bool forward, bool persist) {
    const std::vector<Model> available = m_models.availableModels();
    std::vector<ScopedModel> usable;
    for (const auto& scoped : scopedModels()) {
        if (std::ranges::any_of(available, [&](const Model& model) { return sameModel(model, scoped.model); })) {
            usable.push_back(scoped);
        }
    }
    if (usable.size() <= 1) {
        return std::nullopt;
    }
    const Model current = m_agent.model();
    std::size_t index = 0;
    for (std::size_t i = 0; i < usable.size(); ++i) {
        if (sameModel(usable[i].model, current)) {
            index = i;
        }
    }
    const ScopedModel& next = usable[nextIndex(index, usable.size(), forward)];
    apply(next.model, next.thinkingLevel, persist);
    return ModelCycleResult{next.model, m_agent.thinkingLevel(), true};
}

std::optional<ModelCycleResult> ModelController::cycleAvailable(bool forward, bool persist) {
    const std::vector<Model> available = m_models.availableModels();
    if (available.size() <= 1) {
        return std::nullopt;
    }
    const Model current = m_agent.model();
    std::size_t index = 0;
    for (std::size_t i = 0; i < available.size(); ++i) {
        if (sameModel(available[i], current)) {
            index = i;
        }
    }
    const Model& next = available[nextIndex(index, available.size(), forward)];
    apply(next, std::nullopt, persist);
    return ModelCycleResult{next, m_agent.thinkingLevel(), false};
}

std::optional<ModelCycleResult> ModelController::cycleModel(bool forward, bool persist) {
    return scopedModels().empty() ? cycleAvailable(forward, persist) : cycleScoped(forward, persist);
}
