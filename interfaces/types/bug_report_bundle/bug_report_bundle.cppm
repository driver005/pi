export module pi.types.bug_report_bundle;

import std;
export import pi.types.json;

/** What a bug report holds: metadata and diagnostics (JSON), and optionally the session transcript or a model-written summary. */
export struct BugReportBundle {
    Json metadata = Json::object();
    Json diagnostics = Json::object();
    std::optional<std::string> sessionJsonl;
    std::optional<std::string> summary;
};
