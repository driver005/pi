module;

#include <cstdint>

export module pi.types.session_record;

import std;

/** One hosted session as the server's catalog knows it. */
export struct SessionRecord {
    std::string id;
    std::int64_t createdAt = 0;
    /** The working directory the session runs in. */
    std::string cwd;
    /** The directory holding the session's own files. */
    std::string directory;
};
