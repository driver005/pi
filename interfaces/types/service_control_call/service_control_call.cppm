export module pi.types.service_control_call;

import std;

/** A decoded `$chord.service` control call: catalogue, subscribe or unsubscribe. */
export struct ServiceControlCall {
    /** "catalogue", "subscribe" or "unsubscribe". */
    std::string type;
    std::string subscriptionId;
    std::string serviceId;
    /** "singleton" or "keyed" (subscribe only). */
    std::string mode;
};
