export module pi.types.prepared_commit;

import std;
export import pi.types.document_action;
export import pi.types.json;

/** A commit that passed every storage check and only needs to be applied: nothing in it can fail. */
export struct PreparedCommit {
    std::int64_t seq = 0;
    /** Detached writes with document copies resolved into creations. */
    std::vector<Json> writes;
    std::map<std::int64_t, DocumentAction> actions;
};
