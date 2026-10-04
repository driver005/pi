export module pi.types.provider_state;

import std;
export import pi.types.json;
export import pi.types.model;
export import pi.types.provider_definition;

/** One provider as the runtime currently sees it: identity, merged config, final models. */
export struct ProviderState {
    ProviderDefinition definition;
    /** models.json section overlaid with the plugin registration; null when neither exists. */
    Json config;
    std::vector<Model> models;
};
