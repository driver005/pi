export module pi.provider.i_provider_registry;

import std;
export import pi.provider.i_provider;

/** Wire-API implementations by Model::api name. */
export class IProviderRegistry {
public:
    virtual ~IProviderRegistry() = default;

    /** Replaces any provider registered for the same api. */
    virtual void registerProvider(std::shared_ptr<IProvider> provider) = 0;
    virtual void unregisterProvider(const std::string& api) = 0;
    /** nullptr when no provider handles the api. */
    virtual std::shared_ptr<IProvider> find(const std::string& api) const = 0;
    virtual std::vector<std::string> apis() const = 0;
};
