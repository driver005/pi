module;

#include <nlohmann/json.hpp>

export module pi.types.session_entry;

import std;
export import pi.types.json;

/**
 * One node of the session tree. `body` is the complete entry object as stored in the JSONL file
 * (type, id, parentId, timestamp and the type-specific fields); the other fields are its index
 * keys. Keeping the body verbatim preserves unknown entry types and extension data.
 */
export struct SessionEntry {
    std::string type;
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    Json body = Json::object();
};
