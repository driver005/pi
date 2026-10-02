module;

#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.service_registration;

import std;
export import pi.types.provider_instance;
export import pi.types.provider_subscriber;

/** Everything the provider knows about one published service. */
export struct ServiceRegistration {
    std::string serviceId;
    std::string mode;
    std::shared_ptr<ProviderInstance> singleton;
    /** Member name to "method" or "state" of the current singleton, kept across replacements. */
    std::optional<std::map<std::string, std::string>> singletonShape;
    std::map<std::string, std::shared_ptr<ProviderInstance>> instances;
    std::map<std::string, std::int64_t> generations;
    std::vector<std::shared_ptr<ProviderSubscriber>> subscribers;
};
