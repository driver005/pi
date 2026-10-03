module;

#include <cstdint>

export module pi.support.settings_view;

import std;
export import pi.support.settings_merger;
export import pi.types.assistant_retry_policy;
export import pi.types.compaction_settings;
export import pi.types.json;

/**
 * Typed, defaulted reads over an effective settings object. Values of the wrong type fall back to
 * the default (settings files are not validated). Port of the getters in core/settings-manager.ts
 * that the headless backbone uses; display-only settings (themes, terminal) are not exposed.
 */
export class SettingsView {
public:
    explicit SettingsView(Json settings = Json::object());

    const Json& json() const;

    std::optional<std::string> defaultProvider() const;
    std::optional<std::string> defaultModel() const;
    std::optional<std::string> defaultThinkingLevel() const;
    std::optional<std::string> modelThinkingLevel(const std::string& provider, const std::string& modelId) const;
    std::string steeringMode() const;
    std::string followUpMode() const;
    std::string transport() const;

    bool compactionEnabled() const;
    /** modelOverrides["provider/id"] wins over the plain value; invalid numbers fall back. */
    std::int64_t compactionReserveTokens(const std::string& provider, const std::string& modelId) const;
    std::int64_t compactionKeepRecentTokens(const std::string& provider, const std::string& modelId) const;
    /** enabled, reserve and keep-recent tokens for a model in one value. */
    CompactionSettings compactionSettings(const std::string& provider, const std::string& modelId) const;
    std::int64_t branchSummaryReserveTokens() const;
    bool branchSummarySkipPrompt() const;

    /** settings.retry as a policy for retried assistant calls. */
    AssistantRetryPolicy retryPolicy() const;
    bool retryEnabled() const;
    std::int64_t retryMaxRetries() const;
    std::int64_t retryBaseDelayMs() const;
    std::int64_t retryMaxAgentDelayMs() const;
    std::optional<std::int64_t> providerTimeoutMs() const;
    std::optional<std::int64_t> providerMaxRetries() const;
    std::int64_t providerMaxRetryDelayMs() const;
    std::int64_t httpIdleTimeoutMs() const;
    std::optional<std::int64_t> websocketConnectTimeoutMs() const;
    std::string cacheWarmingMode() const;

    std::optional<std::string> shellPath() const;
    std::optional<std::string> shellCommandPrefix() const;
    std::optional<std::string> sessionDir() const;
    std::optional<std::string> httpProxy() const;
    std::string defaultProjectTrust() const;
    std::vector<std::string> npmCommand() const;

    std::vector<std::string> extensionPaths() const;
    std::vector<std::string> skillPaths() const;
    std::vector<std::string> promptTemplatePaths() const;
    Json packages() const;
    bool enableSkillCommands() const;
    Json thinkingBudgets() const;
    bool imageAutoResize() const;
    bool blockImages() const;
    std::optional<std::vector<std::string>> enabledModels() const;
    std::optional<std::vector<std::string>> defaultTools() const;

private:
    std::optional<std::string> stringAt(const Json& object, const std::string& key) const;
    std::vector<std::string> stringList(const std::string& key) const;
    bool boolAt(const Json& object, const std::string& key, bool fallback) const;
    const Json& section(const std::string& name) const;
    std::optional<std::int64_t> nonNegativeInt(const Json& value) const;
    std::int64_t compactionToken(const std::string& field, const std::string& provider,
                                 const std::string& modelId, std::int64_t fallback) const;

    Json m_settings;
    Json m_empty = Json::object();
    SettingsMerger m_merger;
};

SettingsView::SettingsView(Json settings) : m_settings(settings.is_object() ? std::move(settings) : Json::object()) {}

const Json& SettingsView::json() const {
    return m_settings;
}

const Json& SettingsView::section(const std::string& name) const {
    if (m_settings.contains(name) && m_settings[name].is_object()) {
        return m_settings[name];
    }
    return m_empty;
}

std::optional<std::string> SettingsView::stringAt(const Json& object, const std::string& key) const {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return std::nullopt;
}

bool SettingsView::boolAt(const Json& object, const std::string& key, bool fallback) const {
    if (object.is_object() && object.contains(key) && object[key].is_boolean()) {
        return object[key].get<bool>();
    }
    return fallback;
}

std::optional<std::int64_t> SettingsView::nonNegativeInt(const Json& value) const {
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        return value.get<std::int64_t>();
    }
    return std::nullopt;
}

std::vector<std::string> SettingsView::stringList(const std::string& key) const {
    std::vector<std::string> out;
    if (m_settings.contains(key) && m_settings[key].is_array()) {
        for (const auto& item : m_settings[key]) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
    }
    return out;
}

std::optional<std::string> SettingsView::defaultProvider() const {
    return stringAt(m_settings, "defaultProvider");
}

std::optional<std::string> SettingsView::defaultModel() const {
    return stringAt(m_settings, "defaultModel");
}

std::optional<std::string> SettingsView::defaultThinkingLevel() const {
    return stringAt(m_settings, "defaultThinkingLevel");
}

std::optional<std::string> SettingsView::modelThinkingLevel(const std::string& provider,
                                                            const std::string& modelId) const {
    return stringAt(section("modelThinkingLevels"), provider + "/" + modelId);
}

std::string SettingsView::steeringMode() const {
    const auto mode = stringAt(m_settings, "steeringMode");
    return mode && !mode->empty() ? *mode : "one-at-a-time";
}

std::string SettingsView::followUpMode() const {
    const auto mode = stringAt(m_settings, "followUpMode");
    return mode && !mode->empty() ? *mode : "one-at-a-time";
}

std::string SettingsView::transport() const {
    return stringAt(m_settings, "transport").value_or("auto");
}

bool SettingsView::compactionEnabled() const {
    return boolAt(section("compaction"), "enabled", true);
}

std::int64_t SettingsView::compactionToken(const std::string& field, const std::string& provider,
                                           const std::string& modelId, std::int64_t fallback) const {
    const Json& compaction = section("compaction");
    const std::string key = provider + "/" + modelId;
    if (compaction.contains("modelOverrides") && compaction["modelOverrides"].is_object() &&
        compaction["modelOverrides"].contains(key) && compaction["modelOverrides"][key].is_object()) {
        const Json& override = compaction["modelOverrides"][key];
        if (override.contains(field)) {
            if (const auto value = nonNegativeInt(override[field])) {
                return *value;
            }
        }
    }
    if (compaction.contains(field)) {
        if (const auto value = nonNegativeInt(compaction[field])) {
            return *value;
        }
    }
    return fallback;
}

std::int64_t SettingsView::compactionReserveTokens(const std::string& provider, const std::string& modelId) const {
    return compactionToken("reserveTokens", provider, modelId, 16384);
}

CompactionSettings SettingsView::compactionSettings(const std::string& provider,
                                                    const std::string& modelId) const {
    CompactionSettings out;
    out.enabled = compactionEnabled();
    out.reserveTokens = compactionReserveTokens(provider, modelId);
    out.keepRecentTokens = compactionKeepRecentTokens(provider, modelId);
    return out;
}

std::int64_t SettingsView::compactionKeepRecentTokens(const std::string& provider,
                                                      const std::string& modelId) const {
    return compactionToken("keepRecentTokens", provider, modelId, 20000);
}

std::int64_t SettingsView::branchSummaryReserveTokens() const {
    const Json& branch = section("branchSummary");
    return branch.contains("reserveTokens") && branch["reserveTokens"].is_number() ? branch["reserveTokens"].get<std::int64_t>() : 16384;
}

bool SettingsView::branchSummarySkipPrompt() const {
    return boolAt(section("branchSummary"), "skipPrompt", false);
}

AssistantRetryPolicy SettingsView::retryPolicy() const {
    AssistantRetryPolicy out;
    out.enabled = retryEnabled();
    out.maxRetries = static_cast<int>(retryMaxRetries());
    out.baseDelayMs = retryBaseDelayMs();
    out.maxDelayMs = retryMaxAgentDelayMs();
    return out;
}

bool SettingsView::retryEnabled() const {
    return boolAt(section("retry"), "enabled", true);
}

std::int64_t SettingsView::retryMaxRetries() const {
    const Json& retry = section("retry");
    return retry.contains("maxRetries") && retry["maxRetries"].is_number() ? retry["maxRetries"].get<std::int64_t>() : 3;
}

std::int64_t SettingsView::retryBaseDelayMs() const {
    const Json& retry = section("retry");
    return retry.contains("baseDelayMs") && retry["baseDelayMs"].is_number() ? retry["baseDelayMs"].get<std::int64_t>() : 2000;
}

std::int64_t SettingsView::retryMaxAgentDelayMs() const {
    const Json& retry = section("retry");
    return retry.contains("maxAgentDelayMs") && retry["maxAgentDelayMs"].is_number()
               ? retry["maxAgentDelayMs"].get<std::int64_t>()
               : 60000;
}

std::optional<std::int64_t> SettingsView::providerTimeoutMs() const {
    const Json& retry = section("retry");
    if (retry.contains("provider") && retry["provider"].is_object() && retry["provider"].contains("timeoutMs") &&
        retry["provider"]["timeoutMs"].is_number()) {
        return retry["provider"]["timeoutMs"].get<std::int64_t>();
    }
    return std::nullopt;
}

std::optional<std::int64_t> SettingsView::providerMaxRetries() const {
    const Json& retry = section("retry");
    if (retry.contains("provider") && retry["provider"].is_object() && retry["provider"].contains("maxRetries") &&
        retry["provider"]["maxRetries"].is_number()) {
        return retry["provider"]["maxRetries"].get<std::int64_t>();
    }
    return std::nullopt;
}

std::int64_t SettingsView::providerMaxRetryDelayMs() const {
    const Json& retry = section("retry");
    if (retry.contains("provider") && retry["provider"].is_object() && retry["provider"].contains("maxRetryDelayMs") &&
        retry["provider"]["maxRetryDelayMs"].is_number()) {
        return retry["provider"]["maxRetryDelayMs"].get<std::int64_t>();
    }
    return 60000;
}

std::int64_t SettingsView::httpIdleTimeoutMs() const {
    if (m_settings.contains("httpIdleTimeoutMs") && m_settings["httpIdleTimeoutMs"].is_number()) {
        const double value = m_settings["httpIdleTimeoutMs"].get<double>();
        if (value >= 0) {
            return static_cast<std::int64_t>(value);
        }
    }
    return 300000;
}

std::optional<std::int64_t> SettingsView::websocketConnectTimeoutMs() const {
    if (m_settings.contains("websocketConnectTimeoutMs") && m_settings["websocketConnectTimeoutMs"].is_number()) {
        const double value = m_settings["websocketConnectTimeoutMs"].get<double>();
        if (value >= 0) {
            return static_cast<std::int64_t>(value);
        }
    }
    return std::nullopt;
}

std::string SettingsView::cacheWarmingMode() const {
    const auto mode = stringAt(m_settings, "cacheWarming");
    return mode && (*mode == "off" || *mode == "streaming" || *mode == "idle") ? *mode : "streaming";
}

std::optional<std::string> SettingsView::shellPath() const {
    return stringAt(m_settings, "shellPath");
}

std::optional<std::string> SettingsView::shellCommandPrefix() const {
    return stringAt(m_settings, "shellCommandPrefix");
}

std::optional<std::string> SettingsView::sessionDir() const {
    return stringAt(m_settings, "sessionDir");
}

std::optional<std::string> SettingsView::httpProxy() const {
    return stringAt(m_settings, "httpProxy");
}

std::string SettingsView::defaultProjectTrust() const {
    const auto value = stringAt(m_settings, "defaultProjectTrust");
    return value && (*value == "always" || *value == "never") ? *value : "ask";
}

std::vector<std::string> SettingsView::npmCommand() const {
    return stringList("npmCommand");
}

std::vector<std::string> SettingsView::extensionPaths() const {
    return stringList("extensions");
}

std::vector<std::string> SettingsView::skillPaths() const {
    return stringList("skills");
}

std::vector<std::string> SettingsView::promptTemplatePaths() const {
    return stringList("prompts");
}

Json SettingsView::packages() const {
    return m_settings.contains("packages") && m_settings["packages"].is_array() ? m_settings["packages"]
                                                                                : Json::array();
}

bool SettingsView::enableSkillCommands() const {
    return boolAt(m_settings, "enableSkillCommands", true);
}

Json SettingsView::thinkingBudgets() const {
    return m_settings.contains("thinkingBudgets") && m_settings["thinkingBudgets"].is_object()
               ? m_settings["thinkingBudgets"]
               : Json();
}

bool SettingsView::imageAutoResize() const {
    return boolAt(section("images"), "autoResize", true);
}

bool SettingsView::blockImages() const {
    return boolAt(section("images"), "blockImages", false);
}

std::optional<std::vector<std::string>> SettingsView::enabledModels() const {
    if (!m_settings.contains("enabledModels") || !m_settings["enabledModels"].is_array()) {
        return std::nullopt;
    }
    return stringList("enabledModels");
}

std::optional<std::vector<std::string>> SettingsView::defaultTools() const {
    if (!m_settings.contains("defaultTools")) {
        return std::nullopt;
    }
    return m_merger.resolveDefaultTools(stringList("defaultTools"));
}
