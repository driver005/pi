module;

#include <nlohmann/json.hpp>

export module pi.chord.i_service_provider;

import std;
export import pi.types.json;
export import pi.types.result;
export import pi.types.service_context;
export import pi.types.service_subscription;

/** A host's published services: what it offers, how to call it and how to follow its state. */
export class IServiceProvider {
public:
    using UpdateListener = std::function<void(const Json& update, const ServiceContext& context)>;

    virtual ~IServiceProvider() = default;

    /** `[{"serviceId", "mode"}]`. */
    virtual Json catalogue() const = 0;
    virtual Result<std::optional<Json>> invoke(const Json& call, const ServiceContext& context) = 0;
    /** `mode` is "singleton" or "keyed". The subscription is inert until activate(). */
    virtual Result<ServiceSubscription> subscribe(const std::string& serviceId, const std::string& mode,
                                                  UpdateListener listener) = 0;
};
