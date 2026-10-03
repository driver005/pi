export module pi.ai.memory_models_store;

import std;
export import pi.provider.i_models_store;

/** Process-local IModelsStore. */
export class MemoryModelsStore : public IModelsStore {
public:
    Result<std::optional<ModelsStoreEntry>> read(const std::string& providerId) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_entries.find(providerId);
        if (found == m_entries.end()) {
            return std::optional<ModelsStoreEntry>();
        }
        return std::optional<ModelsStoreEntry>(found->second);
    }

    Result<void> write(const std::string& providerId, const ModelsStoreEntry& entry) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_entries[providerId] = entry;
        return {};
    }

    Result<void> remove(const std::string& providerId) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.erase(providerId);
        return {};
    }

private:
    std::mutex m_mutex;
    std::map<std::string, ModelsStoreEntry> m_entries;
};
