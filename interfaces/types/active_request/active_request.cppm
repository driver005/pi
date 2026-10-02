export module pi.types.active_request;

import std;
export import pi.support.abort_signal;
export import pi.types.rpc_target;

/** A request a connection is working on: its cancellation and the target it was addressed to. */
export struct ActiveRequest {
    std::shared_ptr<AbortSignal> signal;
    RpcTarget target;
};
