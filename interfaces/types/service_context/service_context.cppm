export module pi.types.service_context;

import std;
export import pi.support.abort_signal;

/** What travels with one service call or state change: its cancellation. */
export struct ServiceContext {
    std::shared_ptr<AbortSignal> signal;
};
