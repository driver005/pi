export module pi.types.client_subscription;

import std;
export import pi.types.json;

/** A service subscription as the caller sees it: its id, target and decoded baseline snapshot. */
export struct ClientSubscription {
    std::string id;
    Json target;
    Json snapshot;
};
