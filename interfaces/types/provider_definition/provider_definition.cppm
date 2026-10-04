export module pi.types.provider_definition;

import std;

/** A known model provider: where it lives and how its credentials are found. */
export struct ProviderDefinition {
    std::string id;
    std::string name;
    std::string baseUrl;
    /** Variables holding an API key, in priority order. */
    std::vector<std::string> envVars;
    /** False for providers that only authenticate through OAuth. */
    bool supportsApiKey = true;
    /** True when a subscription/OAuth login exists for the provider. */
    bool supportsOAuth = false;
    /** True for providers shipped with pi; false for ones defined in models.json or a plugin. */
    bool builtin = false;
};
