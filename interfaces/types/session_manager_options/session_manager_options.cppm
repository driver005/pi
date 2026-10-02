module;

#include <nlohmann/json.hpp>

export module pi.types.session_manager_options;

import std;
export import pi.types.json;

/** How a session manager starts: where it lives and what it starts from. */
export struct SessionManagerOptions {
    std::string cwd;
    /** Directory for session files; empty for in-memory sessions. */
    std::string sessionDir;
    /** Existing (or intended) session file to open; nullopt starts a new session. */
    std::optional<std::string> sessionFile;
    bool persist = true;
    /** Explicit id for a new session. */
    std::optional<std::string> id;
    std::optional<std::string> parentSession;
    /** Entries (header first) to start from instead of reading a file. */
    std::vector<Json> preloadedEntries;
};
