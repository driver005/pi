module;

#include <nlohmann/json.hpp>

export module pi.types.session_header;

import std;
export import pi.types.json;

/** First line of a session file. version 1 sessions have no version field. */
export struct SessionHeader {
    int version = 1;
    std::string id;
    std::string timestamp;
    std::string cwd;
    std::optional<std::string> parentSession;
    /** The header object as read, so unknown fields survive rewrites. */
    Json body = Json::object();
};
