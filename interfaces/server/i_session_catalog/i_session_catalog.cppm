export module pi.server.i_session_catalog;

import std;
export import pi.types.result;
export import pi.types.session_record;

/** The sessions a server hosts: listing, creating, resolving ids and removing. Thread-safe. */
export class ISessionCatalog {
public:
    virtual ~ISessionCatalog() = default;

    /** Newest first. */
    virtual Result<std::vector<SessionRecord>> list() = 0;
    /** `id` must be unused and made of letters, digits, '.', '_' and '-'; a fresh id is made when absent. */
    virtual Result<SessionRecord> create(const std::optional<std::string>& id) = 0;
    /** Removes the record and its files; session_not_found when it does not exist. */
    virtual Result<void> remove(const std::string& id) = 0;
    /** The record of an id or unambiguous id prefix; session_not_found / session_ambiguous otherwise. */
    virtual Result<SessionRecord> resolve(const std::string& idOrPrefix) = 0;
};
