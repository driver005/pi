module;

#include <cstdint>

export module pi.types.custom_message;

import std;
export import pi.types.json;

/**
 * Application-defined transcript entry (bashExecution, custom, branchSummary,
 * compactionSummary, ...). `role` names the kind; `data` holds all other JSON fields.
 */
export struct CustomMessage {
    std::string role;
    Json data = Json::object();
    std::int64_t timestamp = 0;
};
