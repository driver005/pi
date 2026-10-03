module;

#include <cstdint>

export module pi.types.output_chunk;

import std;

/** One stored piece of a tool's running output with its byte and newline counts. */
export struct OutputChunk {
    std::string text;
    std::int64_t bytes = 0;
    std::int64_t newlines = 0;
};
