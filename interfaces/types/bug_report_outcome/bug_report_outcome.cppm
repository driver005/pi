export module pi.types.bug_report_outcome;

import std;

/** A filed bug report: its id, how it was delivered and, for a zip, where it is. */
export struct BugReportOutcome {
    std::string id;
    std::string delivery;
    std::optional<std::string> path;
};
