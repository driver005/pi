export module pi.types.document_point;

import std;

/** The current state, or the state as of one historical commit sequence. */
export struct DocumentPoint {
    bool current = true;
    std::int64_t seq = 0;
};
