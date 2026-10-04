module;

#include <cstdint>

export module pi.types.output_slice;

import std;

/** An exact slice of an output within the limits, and what it left out. */
export struct OutputSlice {
    std::string text;
    std::int64_t bytes = 0;
    std::int64_t droppedBytes = 0;
    std::int64_t droppedLines = 0;
};
