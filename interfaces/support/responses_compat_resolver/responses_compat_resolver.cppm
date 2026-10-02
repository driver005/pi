module;

#include <nlohmann/json.hpp>

export module pi.support.responses_compat_resolver;

import std;
export import pi.types.json;
export import pi.types.model;
export import pi.types.responses_compat;

/**
 * Applies the model's explicit `compat` overrides over the OpenAI Responses defaults; the session
 * affinity format is detected from the provider and base URL. Port of getCompat in
 * api/openai-responses.ts (grammar tools, additional tools and tool search are not ported).
 */
export class ResponsesCompatResolver {
public:
    ResponsesCompat resolve(const Model& model) const;

private:
    void overrideBool(bool& target, const Json& overrides, const std::string& key) const;
};

void ResponsesCompatResolver::overrideBool(bool& target, const Json& overrides, const std::string& key) const {
    if (overrides.is_object() && overrides.contains(key) && overrides[key].is_boolean()) {
        target = overrides[key].get<bool>();
    }
}

ResponsesCompat ResponsesCompatResolver::resolve(const Model& model) const {
    ResponsesCompat compat;
    if (model.provider == "openrouter" || model.baseUrl.find("openrouter.ai") != std::string::npos) {
        compat.sessionAffinityFormat = "openrouter";
    }
    const Json& overrides = model.compat;
    overrideBool(compat.supportsDeveloperRole, overrides, "supportsDeveloperRole");
    overrideBool(compat.supportsMidConvoSystemMessages, overrides, "supportsMidConvoSystemMessages");
    overrideBool(compat.supportsLongCacheRetention, overrides, "supportsLongCacheRetention");
    overrideBool(compat.supportsStrictMode, overrides, "supportsStrictMode");
    overrideBool(compat.supportsExplicitPromptCacheMode, overrides, "supportsExplicitPromptCacheMode");
    overrideBool(compat.supportsMaxOutputTokens, overrides, "supportsMaxOutputTokens");
    if (overrides.is_object() && overrides.contains("sessionAffinityFormat") &&
        overrides["sessionAffinityFormat"].is_string()) {
        compat.sessionAffinityFormat = overrides["sessionAffinityFormat"].get<std::string>();
    }
    return compat;
}
