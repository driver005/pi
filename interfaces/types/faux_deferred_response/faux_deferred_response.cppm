export module pi.types.faux_deferred_response;

import std;
export import pi.types.assistant_message;
export import pi.types.deferred_handle;

/** A scripted response the faux provider holds back until its handle has been fetched often enough. */
export struct FauxDeferredResponse {
    DeferredHandle handle;
    /** The final message, with its usage estimated from the request that deferred it. */
    AssistantMessage script;
    /** Fetches that still answer `deferred`. */
    int pendingFetches = 0;
    bool cancelled = false;
};
