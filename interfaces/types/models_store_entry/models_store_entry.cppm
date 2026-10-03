module;

#include <cstdint>

export module pi.types.models_store_entry;

import std;
export import pi.types.json;

/** Persisted remote catalog of one provider (an entry of models-store.json). */
export struct ModelsStoreEntry {
    /** Raw model objects of every type; filtered when applied. */
    Json models = Json::array();
    /** Remote catalog Last-Modified, epoch milliseconds. */
    std::optional<std::int64_t> lastModified;
    /** When the last remote check completed, epoch milliseconds. */
    std::optional<std::int64_t> checkedAt;
    /** ETag verbatim, quotes included. */
    std::optional<std::string> etag;
};
