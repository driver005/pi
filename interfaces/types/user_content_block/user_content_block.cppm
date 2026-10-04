export module pi.types.user_content_block;

import std;
export import pi.types.image_content;
export import pi.types.text_content;

/** Content allowed in user messages and tool results. */
export using UserContentBlock = std::variant<TextContent, ImageContent>;
