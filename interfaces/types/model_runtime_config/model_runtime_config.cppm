export module pi.types.model_runtime_config;

import std;

/** Where the model runtime reads its catalogs from. Empty paths disable that source. */
export struct ModelRuntimeConfig {
    /** User models.json (custom providers, overrides). */
    std::string modelsJsonPath;
    /** Generated catalog directory (models.json or per-provider files). */
    std::string catalogDir;
};
