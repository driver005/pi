export module pi.ai.provider_registry;

import std;
export import pi.provider.i_provider_registry;

/** Thread-safe IProviderRegistry. */
export class ProviderRegistry : public IProviderRegistry {
public:
    void registerProvider(std::shared_ptr<IProvider> provider) override;
    void unregisterProvider(const std::string& api) override;
    std::shared_ptr<IProvider> find(const std::string& api) const override;
    std::vector<std::string> apis() const override;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<IProvider>> m_providers;
};

void ProviderRegistry::registerProvider(std::shared_ptr<IProvider> provider) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_providers[provider->api()] = std::move(provider);
}

void ProviderRegistry::unregisterProvider(const std::string& api) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_providers.erase(api);
}

std::shared_ptr<IProvider> ProviderRegistry::find(const std::string& api) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_providers.find(api);
    return found == m_providers.end() ? nullptr : found->second;
}

std::vector<std::string> ProviderRegistry::apis() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    for (const auto& entry : m_providers) {
        names.push_back(entry.first);
    }
    return names;
}
