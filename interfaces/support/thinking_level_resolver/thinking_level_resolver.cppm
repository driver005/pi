export module pi.support.thinking_level_resolver;

import std;
export import pi.types.json;
export import pi.types.model;
export import pi.types.thinking_level;

/**
 * Which reasoning levels a model offers and how a requested level maps onto them.
 * Port of getSupportedThinkingLevels / clampThinkingLevel in models.ts.
 */
export class ThinkingLevelResolver {
public:
    std::string levelName(ThinkingLevel level) const;
    std::optional<ThinkingLevel> parseLevel(const std::string& name) const;

    /** Off is always first; xhigh/max need an explicit thinkingLevelMap entry; null hides one. */
    std::vector<ThinkingLevel> supportedLevels(const Model& model) const;

    /** Nearest supported level, preferring higher ones. */
    ThinkingLevel clamp(const Model& model, ThinkingLevel level) const;

    /** Provider-native effort string from thinkingLevelMap, or the level name. */
    std::string providerEffort(const Model& model, ThinkingLevel level) const;

private:
    std::vector<ThinkingLevel> allLevels() const;
};

std::vector<ThinkingLevel> ThinkingLevelResolver::allLevels() const {
    return {ThinkingLevel::Off,    ThinkingLevel::Minimal, ThinkingLevel::Low, ThinkingLevel::Medium,
            ThinkingLevel::High,   ThinkingLevel::XHigh,   ThinkingLevel::Max};
}

std::string ThinkingLevelResolver::levelName(ThinkingLevel level) const {
    switch (level) {
        case ThinkingLevel::Off: return "off";
        case ThinkingLevel::Minimal: return "minimal";
        case ThinkingLevel::Low: return "low";
        case ThinkingLevel::Medium: return "medium";
        case ThinkingLevel::High: return "high";
        case ThinkingLevel::XHigh: return "xhigh";
        case ThinkingLevel::Max: return "max";
    }
    return "off";
}

std::optional<ThinkingLevel> ThinkingLevelResolver::parseLevel(const std::string& name) const {
    for (const auto level : allLevels()) {
        if (levelName(level) == name) {
            return level;
        }
    }
    return std::nullopt;
}

std::vector<ThinkingLevel> ThinkingLevelResolver::supportedLevels(const Model& model) const {
    if (!model.reasoning) {
        return {ThinkingLevel::Off};
    }
    std::vector<ThinkingLevel> result;
    for (const auto level : allLevels()) {
        const std::string name = levelName(level);
        const bool hasMap = model.thinkingLevelMap.is_object() && model.thinkingLevelMap.contains(name);
        if (hasMap && model.thinkingLevelMap[name].is_null()) {
            continue;
        }
        if ((level == ThinkingLevel::XHigh || level == ThinkingLevel::Max) && !hasMap) {
            continue;
        }
        result.push_back(level);
    }
    return result;
}

ThinkingLevel ThinkingLevelResolver::clamp(const Model& model, ThinkingLevel level) const {
    const auto available = supportedLevels(model);
    const auto has = [&](ThinkingLevel candidate) {
        return std::find(available.begin(), available.end(), candidate) != available.end();
    };
    if (has(level)) {
        return level;
    }
    const auto levels = allLevels();
    const auto requested = static_cast<std::size_t>(level);
    for (std::size_t i = requested; i < levels.size(); ++i) {
        if (has(levels[i])) {
            return levels[i];
        }
    }
    for (std::size_t i = requested; i-- > 0;) {
        if (has(levels[i])) {
            return levels[i];
        }
    }
    return available.empty() ? ThinkingLevel::Off : available.front();
}

std::string ThinkingLevelResolver::providerEffort(const Model& model, ThinkingLevel level) const {
    const std::string name = levelName(level);
    if (model.thinkingLevelMap.is_object() && model.thinkingLevelMap.contains(name) &&
        model.thinkingLevelMap[name].is_string()) {
        return model.thinkingLevelMap[name].get<std::string>();
    }
    return name;
}
