export module pi.support.model_selector;

import std;
export import pi.support.settings_view;
export import pi.support.thinking_level_resolver;
export import pi.types.model_choice;
export import pi.types.model_selection_input;

/**
 * Picks the starting model and thinking level. The model comes from, in order: the requested
 * model, the model of the restored session, the default model setting, the first model with
 * credentials. The thinking level comes from the request or ":level" suffix, the restored
 * session (only when its model was kept), the per-model and default settings, else medium; it is
 * clamped to what the model supports. Port of findInitialModel in core/model-resolver.ts.
 */
export class ModelSelector {
public:
    ModelChoice select(const ModelSelectionInput& input, const SettingsView& settings) const;

private:
    std::optional<Model> findRequested(const ModelSelectionInput& input, const std::string& text) const;
    std::optional<Model> findAvailable(const std::vector<Model>& models, const std::string& provider,
                                       const std::string& id) const;
    std::optional<Model> firstOf(const ModelSelectionInput& input, const SettingsView& settings,
                                 bool& usedSaved, std::vector<std::string>& warnings) const;
    ThinkingLevel levelFor(const Model& model, std::optional<ThinkingLevel> requested, bool usedSaved,
                           const ModelSelectionInput& input, const SettingsView& settings) const;
    std::string requestedPart(const std::string& text) const;
    std::optional<ThinkingLevel> suffixLevel(const std::string& text) const;

    ThinkingLevelResolver m_levels;
};

ModelChoice ModelSelector::select(const ModelSelectionInput& input, const SettingsView& settings) const {
    ModelChoice choice;
    std::optional<ThinkingLevel> requested;
    if (input.requestedThinking) {
        requested = m_levels.parseLevel(*input.requestedThinking);
        if (!requested) {
            choice.warnings.push_back("Unknown thinking level \"" + *input.requestedThinking + "\"");
        }
    }
    bool usedSaved = false;
    if (input.requestedModel) {
        choice.model = findRequested(input, requestedPart(*input.requestedModel));
        if (choice.model) {
            requested = requested ? requested : suffixLevel(*input.requestedModel);
        } else {
            choice.warnings.push_back("Model \"" + *input.requestedModel + "\" not found");
        }
    }
    if (!choice.model) {
        choice.model = firstOf(input, settings, usedSaved, choice.warnings);
    }
    if (choice.model) {
        choice.thinkingLevel = levelFor(*choice.model, requested, usedSaved, input, settings);
    }
    return choice;
}

std::string ModelSelector::requestedPart(const std::string& text) const {
    return suffixLevel(text) ? text.substr(0, text.find_last_of(':')) : text;
}

std::optional<ThinkingLevel> ModelSelector::suffixLevel(const std::string& text) const {
    const auto colon = text.find_last_of(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    return m_levels.parseLevel(text.substr(colon + 1));
}

std::optional<Model> ModelSelector::findRequested(const ModelSelectionInput& input, const std::string& text) const {
    const auto slash = text.find('/');
    if (slash != std::string::npos) {
        return findAvailable(input.all, text.substr(0, slash), text.substr(slash + 1));
    }
    for (const auto& model : input.all) {
        if (model.id == text) {
            return model;
        }
    }
    return std::nullopt;
}

std::optional<Model> ModelSelector::findAvailable(const std::vector<Model>& models, const std::string& provider,
                                                  const std::string& id) const {
    for (const auto& model : models) {
        if (model.provider == provider && model.id == id) {
            return model;
        }
    }
    return std::nullopt;
}

std::optional<Model> ModelSelector::firstOf(const ModelSelectionInput& input, const SettingsView& settings,
                                            bool& usedSaved, std::vector<std::string>& warnings) const {
    if (input.saved) {
        if (auto model = findAvailable(input.available, input.saved->provider, input.saved->modelId)) {
            usedSaved = true;
            return model;
        }
        warnings.push_back("Could not restore model " + input.saved->provider + "/" + input.saved->modelId);
    }
    if (settings.defaultProvider() && settings.defaultModel()) {
        if (auto model = findAvailable(input.available, *settings.defaultProvider(), *settings.defaultModel())) {
            return model;
        }
    }
    if (!input.available.empty()) {
        return input.available.front();
    }
    warnings.push_back("No models available; add credentials or a models.json provider");
    return std::nullopt;
}

ThinkingLevel ModelSelector::levelFor(const Model& model, std::optional<ThinkingLevel> requested, bool usedSaved,
                                      const ModelSelectionInput& input, const SettingsView& settings) const {
    if (!model.reasoning) {
        return ThinkingLevel::Off;
    }
    std::optional<ThinkingLevel> level = requested;
    if (!level && usedSaved && input.savedThinking) {
        level = m_levels.parseLevel(*input.savedThinking);
    }
    if (!level) {
        if (const auto perModel = settings.modelThinkingLevel(model.provider, model.id)) {
            level = m_levels.parseLevel(*perModel);
        }
    }
    if (!level && settings.defaultThinkingLevel()) {
        level = m_levels.parseLevel(*settings.defaultThinkingLevel());
    }
    return m_levels.clamp(model, level.value_or(ThinkingLevel::Medium));
}
