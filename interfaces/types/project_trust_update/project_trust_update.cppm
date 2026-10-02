export module pi.types.project_trust_update;

import std;

/** Sets (true/false) or forgets (nullopt) the trust decision of one directory. */
export struct ProjectTrustUpdate {
    std::string path;
    std::optional<bool> decision;
};
