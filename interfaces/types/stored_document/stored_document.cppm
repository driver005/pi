export module pi.types.stored_document;

import std;
export import pi.types.json;

/** A document incarnation with its value materialized at a selected point. */
export struct StoredDocument {
    Json record;
    std::int64_t version = 0;
    Json value;
    /** Deltas replayed after the selected base to produce `value`. */
    std::int64_t deltasSinceBase = 0;
};
