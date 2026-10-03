export module pi.types.entry_lookup;

import std;
export import pi.types.json;

/** An entry record and the sequence of the commit that persisted it. */
export struct EntryLookup {
    Json entry;
    std::int64_t commitSeq = 0;
};
