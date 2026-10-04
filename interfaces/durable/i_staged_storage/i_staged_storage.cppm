export module pi.durable.i_staged_storage;

import std;
export import pi.durable.i_storage;
export import pi.types.prepared_commit;

/**
 * A storage whose commit is split in two steps, so a persistent backend can write a commit to disk
 * between validating it and making it visible: prepareCommit() validates and detaches without any
 * observable effect, applyCommit() then adopts it. The caller serialises prepare/apply pairs.
 */
export class IStagedStorage : public IStorage {
public:
    /** `seq` defaults to the next sequence; a given one must strictly increase. */
    virtual Result<PreparedCommit> prepareCommit(const std::vector<Json>& writes, const std::optional<std::int64_t>& seq) = 0;
    virtual Result<std::int64_t> applyCommit(const PreparedCommit& commit) = 0;
};
