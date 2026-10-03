export module pi.types.client_subscription_state;

import std;
export import pi.types.json;

/** A client's side of one service subscription: where updates go and what waits until they may be delivered. */
export struct ClientSubscriptionState {
    Json target;
    std::function<void(const Json& update)> listener;
    /** The snapshot arrived; later updates are decoded directly. */
    bool hydrated = false;
    /** The caller installed the snapshot and started delivery. */
    bool ready = false;
    /** Wire updates that arrived before the snapshot. */
    std::vector<Json> queuedWire;
    /** Decoded updates waiting for start(). */
    std::vector<Json> queued;
};
