module;

#include <cstdint>

export module pi.types.conversation_target;

import std;

/** Where a new conversation comes from: "root", "independent", or a "fork" of `parent` at entry `at`. */
export struct ConversationTarget {
    std::string kind = "independent";
    std::int64_t parent = 0;
    std::int64_t at = 0;
};
