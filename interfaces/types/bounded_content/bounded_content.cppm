module;

#include <cstdint>

export module pi.types.bounded_content;

import std;
export import pi.types.json;

/** Result content after bounding its text, and what the bounds dropped. */
export struct BoundedContent {
    Json content;
    std::int64_t droppedBytes = 0;
    std::int64_t droppedLines = 0;
};
