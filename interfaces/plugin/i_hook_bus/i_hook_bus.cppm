module;

#include <nlohmann/json.hpp>

export module pi.plugin.i_hook_bus;

import std;
export import pi.types.hook_outcome;
export import pi.types.json;
export import pi.types.result;

/**
 * Named events the session fires and plugins subscribe to (`tool_call`, `tool_result`, `context`,
 * agent events, ...). Handlers run synchronously on the thread that fires the event, in
 * subscription order. Thread-safe.
 */
export class IHookBus {
public:
    /** A handler returns its result, or null for "no opinion". */
    using Handler = std::function<Result<Json>(const std::string& event, const Json& payload)>;
    /** Folds one handler result into the payload the next handler will see. */
    using Apply = std::function<void(Json& payload, const Json& result)>;

    virtual ~IHookBus() = default;

    /** Returns an id for unsubscribe(). */
    virtual std::uint64_t subscribe(const std::string& event, Handler handler) = 0;
    virtual void unsubscribe(std::uint64_t id) = 0;
    virtual bool hasHandlers(const std::string& event) const = 0;

    /** Calls every handler of `event` with the payload; `apply` (optional) chains the results. */
    virtual HookOutcome emit(const std::string& event, const Json& payload, const Apply& apply = {}) = 0;
};
