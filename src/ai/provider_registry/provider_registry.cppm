export module pi.ai.provider_registry;

import std;
export import pi.provider.i_provider_registry;

/** Thread-safe IProviderRegistry. */
export class ProviderRegistry : public IProviderRegistry {
public:
    void registerProvider(std::shared_ptr<IProvider> provider) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_providers[provider->api()] = std::move(provider);
    }

    void unregisterProvider(const std::string& api) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_providers.erase(api);
    }

    std::shared_ptr<IProvider> find(const std::string& api) const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_providers.find(api);
        return found == m_providers.end() ? nullptr : found->second;
    }

    std::vector<std::string> apis() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> names;
        for (const auto& entry : m_providers) {
            names.push_back(entry.first);
        }
        return names;
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<IProvider>> m_providers;
};
