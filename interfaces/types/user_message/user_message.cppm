module;

#include <cstdint>

export module pi.types.user_message;

import std;
export import pi.types.user_content_block;

export struct UserMessage {
    std::variant<std::string, std::vector<UserContentBlock>> content = std::string();
    std::int64_t timestamp = 0;
};
