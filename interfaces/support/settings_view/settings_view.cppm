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
    explicit SettingsView(Json settings = Json::object())
        : m_settings(settings.is_object() ? std::move(settings) : Json::object()) {}

    const Json& json() const {
        return m_settings;
    }

    std::optional<std::string> defaultProvider() const {
        return stringAt(m_settings, "defaultProvider");
    }

    std::optional<std::string> defaultModel() const {
        return stringAt(m_settings, "defaultModel");
    }

    std::optional<std::string> defaultThinkingLevel() const {
        return stringAt(m_settings, "defaultThinkingLevel");
    }

    std::optional<std::string> modelThinkingLevel(const std::string& provider, const std::string& modelId) const {
        return stringAt(section("modelThinkingLevels"), provider + "/" + modelId);
    }

    std::string steeringMode() const {
        const auto mode = stringAt(m_settings, "steeringMode");
        return mode && !mode->empty() ? *mode : "one-at-a-time";
    }

    std::string followUpMode() const {
        const auto mode = stringAt(m_settings, "followUpMode");
        return mode && !mode->empty() ? *mode : "one-at-a-time";
    }

    std::string transport() const {
        return stringAt(m_settings, "transport").value_or("auto");
    }

    bool compactionEnabled() const {
        return boolAt(section("compaction"), "enabled", true);
    }

    /** modelOverrides["provider/id"] wins over the plain value; invalid numbers fall back. */
    std::int64_t compactionReserveTokens(const std::string& provider, const std::string& modelId) const {
        return compactionToken("reserveTokens", provider, modelId, 16384);
    }

    std::int64_t compactionKeepRecentTokens(const std::string& provider, const std::string& modelId) const {
        return compactionToken("keepRecentTokens", provider, modelId, 20000);
    }

    /** enabled, reserve and keep-recent tokens for a model in one value. */
    CompactionSettings compactionSettings(const std::string& provider, const std::string& modelId) const {
        CompactionSettings out;
        out.enabled = compactionEnabled();
        out.reserveTokens = compactionReserveTokens(provider, modelId);
        out.keepRecentTokens = compactionKeepRecentTokens(provider, modelId);
        return out;
    }

    std::int64_t branchSummaryReserveTokens() const {
        const Json& branch = section("branchSummary");
        return branch.contains("reserveTokens") && branch["reserveTokens"].is_number() ? branch["reserveTokens"].get<std::int64_t>() : 16384;
    }

    bool branchSummarySkipPrompt() const {
        return boolAt(section("branchSummary"), "skipPrompt", false);
    }

    /** settings.retry as a policy for retried assistant calls. */
    AssistantRetryPolicy retryPolicy() const {
        AssistantRetryPolicy out;
        out.enabled = retryEnabled();
        out.maxRetries = static_cast<int>(retryMaxRetries());
        out.baseDelayMs = retryBaseDelayMs();
        out.maxDelayMs = retryMaxAgentDelayMs();
        return out;
    }

    bool retryEnabled() const {
        return boolAt(section("retry"), "enabled", true);
    }

    std::int64_t retryMaxRetries() const {
        const Json& retry = section("retry");
        return retry.contains("maxRetries") && retry["maxRetries"].is_number() ? retry["maxRetries"].get<std::int64_t>() : 3;
    }

    std::int64_t retryBaseDelayMs() const {
        const Json& retry = section("retry");
        return retry.contains("baseDelayMs") && retry["baseDelayMs"].is_number() ? retry["baseDelayMs"].get<std::int64_t>() : 2000;
    }

    std::int64_t retryMaxAgentDelayMs() const {
        const Json& retry = section("retry");
        return retry.contains("maxAgentDelayMs") && retry["maxAgentDelayMs"].is_number()
                   ? retry["maxAgentDelayMs"].get<std::int64_t>()
                   : 60000;
    }

    std::optional<std::int64_t> providerTimeoutMs() const {
        const Json& retry = section("retry");
        if (retry.contains("provider") && retry["provider"].is_object() && retry["provider"].contains("timeoutMs") &&
            retry["provider"]["timeoutMs"].is_number()) {
            return retry["provider"]["timeoutMs"].get<std::int64_t>();
        }
        return std::nullopt;
    }

    std::optional<std::int64_t> providerMaxRetries() const {
        const Json& retry = section("retry");
        if (retry.contains("provider") && retry["provider"].is_object() && retry["provider"].contains("maxRetries") &&
            retry["provider"]["maxRetries"].is_number()) {
            return retry["provider"]["maxRetries"].get<std::int64_t>();
        }
        return std::nullopt;
    }

    std::int64_t providerMaxRetryDelayMs() const {
        const Json& retry = section("retry");
        if (retry.contains("provider") && retry["provider"].is_object() && retry["provider"].contains("maxRetryDelayMs") &&
            retry["provider"]["maxRetryDelayMs"].is_number()) {
            return retry["provider"]["maxRetryDelayMs"].get<std::int64_t>();
        }
        return 60000;
    }

    std::int64_t httpIdleTimeoutMs() const {
        if (m_settings.contains("httpIdleTimeoutMs") && m_settings["httpIdleTimeoutMs"].is_number()) {
            const double value = m_settings["httpIdleTimeoutMs"].get<double>();
            if (value >= 0) {
                return static_cast<std::int64_t>(value);
            }
        }
        return 300000;
    }

    std::optional<std::int64_t> websocketConnectTimeoutMs() const {
        if (m_settings.contains("websocketConnectTimeoutMs") && m_settings["websocketConnectTimeoutMs"].is_number()) {
            const double value = m_settings["websocketConnectTimeoutMs"].get<double>();
            if (value >= 0) {
                return static_cast<std::int64_t>(value);
            }
        }
        return std::nullopt;
    }

    std::string cacheWarmingMode() const {
        const auto mode = stringAt(m_settings, "cacheWarming");
        return mode && (*mode == "off" || *mode == "streaming" || *mode == "idle") ? *mode : "streaming";
    }

    std::optional<std::string> shellPath() const {
        return stringAt(m_settings, "shellPath");
    }

    std::optional<std::string> shellCommandPrefix() const {
        return stringAt(m_settings, "shellCommandPrefix");
    }

    std::optional<std::string> sessionDir() const {
        return stringAt(m_settings, "sessionDir");
    }

    std::optional<std::string> httpProxy() const {
        return stringAt(m_settings, "httpProxy");
    }

    std::string defaultProjectTrust() const {
        const auto value = stringAt(m_settings, "defaultProjectTrust");
        return value && (*value == "always" || *value == "never") ? *value : "ask";
    }

    std::vector<std::string> npmCommand() const {
        return stringList("npmCommand");
    }

    std::vector<std::string> extensionPaths() const {
        return stringList("extensions");
    }

    std::vector<std::string> skillPaths() const {
        return stringList("skills");
    }

    std::vector<std::string> promptTemplatePaths() const {
        return stringList("prompts");
    }

    Json packages() const {
        return m_settings.contains("packages") && m_settings["packages"].is_array() ? m_settings["packages"]
                                                                                    : Json::array();
    }

    bool enableSkillCommands() const {
        return boolAt(m_settings, "enableSkillCommands", true);
    }

    Json thinkingBudgets() const {
        return m_settings.contains("thinkingBudgets") && m_settings["thinkingBudgets"].is_object()
                   ? m_settings["thinkingBudgets"]
                   : Json();
    }

    bool imageAutoResize() const {
        return boolAt(section("images"), "autoResize", true);
    }

    bool blockImages() const {
        return boolAt(section("images"), "blockImages", false);
    }

    std::optional<std::vector<std::string>> enabledModels() const {
        if (!m_settings.contains("enabledModels") || !m_settings["enabledModels"].is_array()) {
            return std::nullopt;
        }
        return stringList("enabledModels");
    }

    std::optional<std::vector<std::string>> defaultTools() const {
        if (!m_settings.contains("defaultTools")) {
            return std::nullopt;
        }
        return m_merger.resolveDefaultTools(stringList("defaultTools"));
    }

private:
    std::optional<std::string> stringAt(const Json& object, const std::string& key) const {
        if (object.is_object() && object.contains(key) && object[key].is_string()) {
            return object[key].get<std::string>();
        }
        return std::nullopt;
    }

    std::vector<std::string> stringList(const std::string& key) const {
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

    bool boolAt(const Json& object, const std::string& key, bool fallback) const {
        if (object.is_object() && object.contains(key) && object[key].is_boolean()) {
            return object[key].get<bool>();
        }
        return fallback;
    }

    const Json& section(const std::string& name) const {
        if (m_settings.contains(name) && m_settings[name].is_object()) {
            return m_settings[name];
        }
        return m_empty;
    }

    std::optional<std::int64_t> nonNegativeInt(const Json& value) const {
        if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
            return value.get<std::int64_t>();
        }
        return std::nullopt;
    }

    std::int64_t compactionToken(const std::string& field, const std::string& provider, const std::string& modelId, std::int64_t fallback) const {
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

    Json m_settings;
    Json m_empty = Json::object();
    SettingsMerger m_merger;
};
