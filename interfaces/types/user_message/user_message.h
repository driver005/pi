#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "interfaces/types/user_content_block/user_content_block.h"

struct UserMessage {
    std::variant<std::string, std::vector<UserContentBlock>> content = std::string();
    std::int64_t timestamp = 0;
};
