export module pi.types.virtual_route_request;

import std;
export import pi.support.abort_signal;
export import pi.types.failed_selection;
export import pi.types.json;
export import pi.types.message;
export import pi.types.model;
export import pi.types.routed_selection;
export import pi.types.thinking_level;

/**
 * What a virtual model's router is asked for one request. `reason` is why the request is made: "user" (first request after a
 * message the user wrote), "continuation" (any other request of the agent loop), "retry" (automatic retry after a failed
 * request) or "direct" (a request outside the agent loop, such as a compaction summary).
 */
export struct VirtualRouteRequest {
    /** The selected virtual model and thinking level. */
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    std::string reason = "user";
    /** Physical model and level of the latest successful response in `messages`. */
    std::optional<RoutedSelection> previous;
    /** For a retry: the request that failed; absent when routing itself failed. */
    std::optional<FailedSelection> failed;
    /** Router state last returned on this session branch; null before the first state and for direct requests. */
    Json state;
    std::vector<Message> messages;
    std::shared_ptr<AbortSignal> signal;
};
