module;

#include <nlohmann/json.hpp>

export module pi.chord.i_service_endpoint;

import std;
export import pi.types.json;
export import pi.types.result;
export import pi.types.service_context;

/**
 * One remote consumer's view of a provider: calls (including the `$chord.service` control calls
 * catalogue/subscribe/unsubscribe) and the subscriptions it opened. dispose() closes them all.
 */
export class IServiceEndpoint {
public:
    /** Receives (subscriptionId, update) for every update of a subscription opened through this endpoint. */
    using Publisher = std::function<void(const std::string& subscriptionId, const Json& update, const ServiceContext& context)>;

    virtual ~IServiceEndpoint() = default;

    virtual Result<std::optional<Json>> invoke(const Json& call, const Publisher& publish, const ServiceContext& context) = 0;
    virtual void dispose() = 0;
};
