module;

#include <cstdint>

export module pi.types.bounded_output;

import std;

/** Retained output and what the limits dropped. */
export struct BoundedOutput {
    std::string text;
    std::int64_t droppedBytes = 0;
    std::int64_t droppedLines = 0;
};
