export module pi.provider.i_models_store;

import std;
export import pi.types.models_store_entry;
export import pi.types.result;

/** Persistent per-provider remote model catalogs (models-store.json). */
export class IModelsStore {
public:
    virtual ~IModelsStore() = default;

    virtual Result<std::optional<ModelsStoreEntry>> read(const std::string& providerId) = 0;
    virtual Result<void> write(const std::string& providerId, const ModelsStoreEntry& entry) = 0;
    virtual Result<void> remove(const std::string& providerId) = 0;
};
