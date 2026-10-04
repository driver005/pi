export module pi.session.i_project_trust_store;

import std;
export import pi.types.project_trust_entry;
export import pi.types.project_trust_update;
export import pi.types.result;

/** Per-directory trust decisions (<agentDir>/trust.json); the nearest decision on the cwd's ancestor chain applies. */
export class IProjectTrustStore {
public:
    virtual ~IProjectTrustStore() = default;

    /** true/false when a decision exists for the cwd or an ancestor, nullopt otherwise. */
    virtual Result<std::optional<bool>> get(const std::string& cwd) = 0;
    virtual Result<std::optional<ProjectTrustEntry>> getEntry(const std::string& cwd) = 0;
    virtual Result<void> set(const std::string& cwd, std::optional<bool> decision) = 0;
    virtual Result<void> setMany(const std::vector<ProjectTrustUpdate>& updates) = 0;
};
