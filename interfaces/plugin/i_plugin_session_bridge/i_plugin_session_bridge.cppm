export module pi.plugin.i_plugin_session_bridge;

import std;
export import pi.support.abort_signal;
export import pi.types.json;

/**
 * What plugins can do to the session they run in (PiHostApi.session_call): one named operation with JSON arguments and a JSON
 * result, or `{"error": "..."}`. Implemented over the live session; before it exists the host answers on its own.
 */
export class IPluginSessionBridge {
public:
    virtual ~IPluginSessionBridge() = default;

    /** `abort` may be null; waiting methods stop when it fires. */
    virtual Json call(const std::string& method, const Json& params, const AbortSignal* abort) = 0;
};
