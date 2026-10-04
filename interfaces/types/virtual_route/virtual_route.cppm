export module pi.types.virtual_route;

import std;
export import pi.types.json;
export import pi.types.model;
export import pi.types.thinking_level;

/** The physical model and thinking level a router picked for one request, and optionally a new router state. */
export struct VirtualRoute {
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    /** New state to store on the session branch; nullopt keeps the current one. */
    std::optional<Json> state;
};
