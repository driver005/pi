export module pi.types.text_content;

import std;

/** Plain text block. textSignature carries provider message metadata (e.g. OpenAI Responses). */
export struct TextContent {
    std::string text;
    std::optional<std::string> textSignature;
};
