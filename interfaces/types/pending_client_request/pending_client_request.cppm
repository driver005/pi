export module pi.types.pending_client_request;

import std;
export import pi.types.error;
export import pi.types.json;
export import pi.types.result;

/** A request the client sent and has no answer for yet. `transform` (optional) runs on the response's result in order. */
export struct PendingClientRequest {
    bool done = false;
    std::optional<Json> result;
    std::optional<Error> error;
    std::function<Result<Json>(const Json& result)> transform;
};
