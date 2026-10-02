#pragma once

#include <variant>

#include "interfaces/types/image_content/image_content.h"
#include "interfaces/types/text_content/text_content.h"

/** Content allowed in user messages and tool results. */
using UserContentBlock = std::variant<TextContent, ImageContent>;
