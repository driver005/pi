#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "interfaces/types/json/json.h"

/** Durable handle for a response a provider continues asynchronously. */
struct DeferredHandle {
    std::string provider;
    std::string modelId;
    std::string api;
    std::string id;
    std::optional<std::int64_t> expiresAt;
    std::optional<std::int64_t> pollAfterMs;
    /** Provider conversion data; null means absent. */
    Json data;
};
