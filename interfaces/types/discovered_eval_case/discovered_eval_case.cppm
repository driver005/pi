export module pi.types.discovered_eval_case;

import std;

/** One documentation eval case found by Vitest discovery: its file and its `<eval set> > <case>` name split into identity parts. */
export struct DiscoveredEvalCase {
    std::string file;
    std::string fullName;
    std::string evalSet;
    std::string caseId;
};
