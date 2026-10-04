export module pi.types.project_trust_entry;

import std;

/** A stored trust decision and the directory it was recorded for (maybe an ancestor of the cwd). */
export struct ProjectTrustEntry {
    std::string path;
    bool decision = false;
};
