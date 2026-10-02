module;

#include <nlohmann/json.hpp>

export module pi.types.pending_subscription;

import std;
export import pi.types.json;

/**
 * Updates of a subscription that arrive before its snapshot response has been sent; they are held
 * back and sent right after it, in order.
 */
export struct PendingSubscription {
    std::mutex mutex;
    std::string subscriptionId;
    bool ready = true;
    std::vector<Json> updates;
};
