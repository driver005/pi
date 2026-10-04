export module pi.types.service_subscription;

import std;
export import pi.types.json;

/**
 * A provider-side subscription: the atomic baseline snapshot, then updates buffered until activate()
 * (a full reset replaces the buffer after 100 pending updates), until close().
 */
export struct ServiceSubscription {
    Json snapshot;
    std::function<void()> activate;
    std::function<void()> close;
};
