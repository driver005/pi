export module pi.types.bug_report_request;

import std;

/** What the user asked for when filing a bug report. */
export struct BugReportRequest {
    std::optional<std::string> hint;
    /** Attach the session transcript. */
    bool includeSession = false;
    /** Attach a summary the session model writes (only meaningful without the transcript). */
    bool includeSummary = false;
    /** "upload" (to the Radius gateway) or "zip" (a zip archive on disk). */
    std::string delivery = "zip";
    /** zip: the file to write or a directory to put `pi-bug-report-<id>.zip` in; the working directory by default. */
    std::optional<std::string> outputPath;
};
