#pragma once

#include <cstdint>
#include <string>

#include "interfaces/types/json/json.h"

/**
 * Application-defined transcript entry (bashExecution, custom, branchSummary,
 * compactionSummary, ...). `role` names the kind; `data` holds all other JSON fields.
 */
struct CustomMessage {
    std::string role;
    Json data = Json::object();
    std::int64_t timestamp = 0;
};
