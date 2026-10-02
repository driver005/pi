module;
#include <nlohmann/json.hpp>

#include <cstdint>

export module pi.types.deferred_handle;

import std;
export import pi.types.json;

/** Durable handle for a response a provider continues asynchronously. */
export struct DeferredHandle {
    std::string provider;
    std::string modelId;
    std::string api;
    std::string id;
    std::optional<std::int64_t> expiresAt;
    std::optional<std::int64_t> pollAfterMs;
    /** Provider conversion data; null means absent. */
    Json data;
};
