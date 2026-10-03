module;

#include <cstdint>

export module pi.types.output_limits;

import std;

/** Bounds of the text a tool call retains: whole bytes and lines, kept from the head or the tail. */
export struct OutputLimits {
    std::int64_t maxBytes = 50 * 1024;
    std::int64_t maxLines = 2000;
    /** "head" or "tail". */
    std::string retain = "head";
};
