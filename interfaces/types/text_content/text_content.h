#pragma once

#include <optional>
#include <string>

/** Plain text block. textSignature carries provider message metadata (e.g. OpenAI Responses). */
struct TextContent {
    std::string text;
    std::optional<std::string> textSignature;
};
