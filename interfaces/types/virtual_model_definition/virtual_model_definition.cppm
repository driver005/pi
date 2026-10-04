module;

#include <cstdint>

export module pi.types.virtual_model_definition;

import std;
export import pi.types.result;
export import pi.types.thinking_level;
export import pi.types.virtual_route;
export import pi.types.virtual_route_request;

/**
 * A selectable model that picks a physical model for each request. `route` runs before every request made with it and
 * returns a catalog model of a provider with credentials (never another virtual model) and a thinking level.
 */
export struct VirtualModelDefinition {
    /** Provider the model is listed under; may also list physical models or other virtual models. */
    std::string provider;
    /** Must not be the id of a physical model of `provider`. */
    std::string id;
    std::string name;
    /** Levels offered for selection; empty means just "off". */
    std::vector<ThinkingLevel> thinkingLevels;
    /** Limits shown before the first response; 0 is unknown. */
    std::int64_t contextWindow = 0;
    std::int64_t maxTokens = 0;
    /** Input types offered for selection; empty means text and images. */
    std::vector<std::string> input;
    std::function<Result<VirtualRoute>(const VirtualRouteRequest&)> route;
};
