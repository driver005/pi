export module pi.types.cut_point_result;

import std;

/** Where compaction cuts the projected entries. */
export struct CutPointResult {
    /** Index of the first entry to keep. */
    std::size_t firstKeptEntryIndex = 0;
    /** Entry that starts the turn being split, or -1 when the cut is at a turn start. */
    int turnStartIndex = -1;
    /** The cut falls inside a turn (the kept part starts at a non-turn-start message). */
    bool isSplitTurn = false;
};
