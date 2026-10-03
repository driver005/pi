module;

#include <cstdint>

export module pi.types.doc_definition;

import std;
export import pi.types.json;

/**
 * Definition of a durable document (a singleton, or a keyed family): its persisted kind and value
 * version, ownership and history semantics, and how to create, migrate and checkpoint it. Values are
 * JSON objects. Port of DocDefinition / DocFamilyDefinition in packages/durable/src/types.ts.
 */
export struct DocDefinition {
    /** Stable persisted kind; part of the public protocol. */
    std::string kind;
    /** Positive version of the stored value shape. */
    std::int64_t version = 1;
    /** "session", "conversation" or "task". */
    std::string scope = "session";
    /** Conversation documents: "latest" or "rewindable". */
    std::string history;
    /** Conversation documents: "current" or "initial" (latest), also "asOf" (rewindable). */
    std::string fork;
    /** A keyed family: `initial` receives the seed, and every access names a key. */
    bool family = false;
    /** The value of a member that does not exist yet; the seed is null for singletons. */
    std::function<Json(const Json& seed)> initial;
    /** Converts a stored value of an older version; absent when older versions are unsupported. */
    std::function<Json(const Json& value, std::int64_t fromVersion)> migrate;
    /** True to store this ordinary change as a complete base; gets the new value, its ops, deltas since the base. */
    std::function<bool(const Json& value, const Json& ops, std::int64_t deltasSinceBase)> checkpointWhen;
};
