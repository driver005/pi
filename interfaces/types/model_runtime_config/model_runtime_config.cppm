export module pi.types.model_runtime_config;

import std;

/** Where the model runtime reads its catalogs from. Empty paths disable that source. */
export struct ModelRuntimeConfig {
    /** User models.json (custom providers, overrides). */
    std::string modelsJsonPath;
    /** Generated catalog directory (models.json or per-provider files). */
    std::string catalogDir;
    /** Default User-Agent of provider requests (PiUserAgent); a header the model, the provider config or the caller sets wins. */
    std::string userAgent = "pi";
};
