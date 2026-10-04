module;

#include <cstdint>

export module pi.types.device_poll_result;

import std;
export import pi.types.json;

/** One poll of a device flow: `status` is "pending", "slow_down", "failed" (with `message`) or "complete" (with `value`). */
export struct DevicePollResult {
    std::string status = "pending";
    /** The new polling interval a `slow_down` answer asked for. */
    std::optional<std::int64_t> intervalSeconds;
    std::string message;
    Json value;
};
