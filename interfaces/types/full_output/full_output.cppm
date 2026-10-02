export module pi.types.full_output;

import std;

/** The complete output of a command, possibly with the middle omitted. */
export struct FullOutput {
    std::string content;
    /** True when `content` omits part of the output. */
    bool truncated = false;
};
