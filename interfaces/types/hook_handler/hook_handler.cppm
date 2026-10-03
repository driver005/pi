export module pi.types.hook_handler;

import std;
export import pi.durable.i_hook_api;
export import pi.types.json;
export import pi.types.result;

/** One hook handler: takes a JSON payload (its shape depends on the hook) and may return a JSON decision. */
export using HookHandler = std::function<Result<std::optional<Json>>(const Json& payload, IHookApi& api)>;
