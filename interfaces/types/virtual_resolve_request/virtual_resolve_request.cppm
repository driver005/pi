export module pi.types.virtual_resolve_request;

import std;
export import pi.support.abort_signal;
export import pi.types.assistant_message;
export import pi.types.json;
export import pi.types.message;
export import pi.types.model;
export import pi.types.thinking_level;

/**
 * A request to route one call of a virtual model: the model runtime computes the `previous` selection from `messages`,
 * turns `failed` into the router's failed selection and checks and clamps the router's answer.
 */
export struct VirtualResolveRequest {
    /** The selected virtual model and thinking level. */
    Model model;
    ThinkingLevel thinkingLevel = ThinkingLevel::Off;
    /** "user", "continuation", "retry" or "direct" (see VirtualRouteRequest). */
    std::string reason = "user";
    /** For a retry: the assistant message of the failed request, which `messages` no longer contains. */
    std::optional<AssistantMessage> failed;
    /** Router state stored on the session branch; null when none. */
    Json state;
    std::vector<Message> messages;
    std::shared_ptr<AbortSignal> signal;
};
