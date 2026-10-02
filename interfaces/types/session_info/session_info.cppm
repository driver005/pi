module;

#include <cstdint>

export module pi.types.session_info;

import std;

/** Summary of a session file for listings. */
export struct SessionInfo {
    std::string path;
    std::string id;
    /** Working directory the session started in; empty for old sessions. */
    std::string cwd;
    std::optional<std::string> name;
    std::optional<std::string> parentSessionPath;
    std::int64_t createdMs = 0;
    std::int64_t modifiedMs = 0;
    int messageCount = 0;
    std::string firstMessage;
    std::string allMessagesText;
};
