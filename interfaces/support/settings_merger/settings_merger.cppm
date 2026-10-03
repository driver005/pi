export module pi.support.settings_merger;

import std;
export import pi.types.json;

/**
 * Settings layering: legacy-format migration, deep merge of the global and project layers and
 * the defaultTools list algebra. Port of the helpers at the top of core/settings-manager.ts.
 */
export class SettingsMerger {
public:
    /** Tools enabled when defaultTools does not change them. */
    std::vector<std::string> defaultToolNames() const;

    /** queueMode -> steeringMode, websockets -> transport, skills object -> array, retry.maxDelayMs. */
    Json migrate(Json settings) const;

    /** overrides win; nested objects merge recursively; defaultTools has its own rule. */
    Json merge(const Json& base, const Json& overrides) const;

    /** Plain names replace the defaults; "+name"/"-name" entries edit the list in order. */
    std::vector<std::string> resolveDefaultTools(const std::vector<std::string>& entries) const;

private:
    bool isToolModifier(const Json& entry) const;
    Json mergeObjects(const Json& base, const Json& overrides) const;
    Json mergeDefaultTools(const Json& base, const Json& overrides) const;
};

std::vector<std::string> SettingsMerger::defaultToolNames() const {
    return {"read", "bash", "edit", "write"};
}

bool SettingsMerger::isToolModifier(const Json& entry) const {
    if (!entry.is_string()) {
        return false;
    }
    const std::string text = entry.get<std::string>();
    return !text.empty() && (text[0] == '+' || text[0] == '-');
}

Json SettingsMerger::mergeObjects(const Json& base, const Json& overrides) const {
    Json result = base;
    for (const auto& entry : overrides.items()) {
        const std::string& key = entry.key();
        const Json& value = entry.value();
        if (result.contains(key) && result[key].is_object() && value.is_object()) {
            result[key] = mergeObjects(result[key], value);
        } else {
            result[key] = value;
        }
    }
    return result;
}

Json SettingsMerger::mergeDefaultTools(const Json& base, const Json& overrides) const {
    if (!base.is_array() || !overrides.is_array() ||
        !std::all_of(overrides.begin(), overrides.end(), [this](const Json& item) { return isToolModifier(item); })) {
        return overrides;
    }
    Json merged = base;
    for (const auto& entry : overrides) {
        merged.push_back(entry);
    }
    return merged;
}

Json SettingsMerger::merge(const Json& base, const Json& overrides) const {
    Json merged = mergeObjects(base.is_object() ? base : Json::object(),
                               overrides.is_object() ? overrides : Json::object());
    if (overrides.is_object() && overrides.contains("defaultTools")) {
        const Json baseTools = base.is_object() && base.contains("defaultTools") ? base["defaultTools"] : Json();
        merged["defaultTools"] = baseTools.is_null() ? overrides["defaultTools"]
                                                     : mergeDefaultTools(baseTools, overrides["defaultTools"]);
    }
    return merged;
}

std::vector<std::string> SettingsMerger::resolveDefaultTools(const std::vector<std::string>& entries) const {
    std::vector<std::string> plain;
    for (const auto& entry : entries) {
        if (!isToolModifier(Json(entry))) {
            plain.push_back(entry);
        }
    }
    std::vector<std::string> tools =
        !plain.empty() || entries.empty() ? plain : defaultToolNames();
    for (const auto& entry : entries) {
        if (!isToolModifier(Json(entry))) {
            continue;
        }
        const std::string name = entry.substr(1);
        const auto found = std::find(tools.begin(), tools.end(), name);
        if (entry[0] == '+' && found == tools.end() && !name.empty()) {
            tools.push_back(name);
        } else if (entry[0] == '-' && found != tools.end()) {
            tools.erase(found);
        }
    }
    return tools;
}

Json SettingsMerger::migrate(Json settings) const {
    if (!settings.is_object()) {
        return settings;
    }
    if (settings.contains("queueMode") && !settings.contains("steeringMode")) {
        settings["steeringMode"] = settings["queueMode"];
        settings.erase("queueMode");
    }
    if (!settings.contains("transport") && settings.contains("websockets") && settings["websockets"].is_boolean()) {
        settings["transport"] = settings["websockets"].get<bool>() ? "websocket" : "sse";
        settings.erase("websockets");
    }
    if (settings.contains("skills") && settings["skills"].is_object()) {
        const Json skills = settings["skills"];
        if (skills.contains("enableSkillCommands") && !settings.contains("enableSkillCommands")) {
            settings["enableSkillCommands"] = skills["enableSkillCommands"];
        }
        if (skills.contains("customDirectories") && skills["customDirectories"].is_array() &&
            !skills["customDirectories"].empty()) {
            settings["skills"] = skills["customDirectories"];
        } else {
            settings.erase("skills");
        }
    }
    if (settings.contains("retry") && settings["retry"].is_object()) {
        Json& retry = settings["retry"];
        const bool providerHas = retry.contains("provider") && retry["provider"].is_object() &&
                                 retry["provider"].contains("maxRetryDelayMs") &&
                                 !retry["provider"]["maxRetryDelayMs"].is_null();
        if (retry.contains("maxDelayMs") && retry["maxDelayMs"].is_number() && !providerHas) {
            Json provider = retry.contains("provider") && retry["provider"].is_object() ? retry["provider"] : Json::object();
            provider["maxRetryDelayMs"] = retry["maxDelayMs"];
            retry["provider"] = std::move(provider);
        }
        retry.erase("maxDelayMs");
    }
    return settings;
}
