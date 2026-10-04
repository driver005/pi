export module pi.types.bug_report_file;

import std;

/** A file of a bug report, shared by the upload form and the zip export. */
export struct BugReportFile {
    std::string name;
    std::string contentType;
    std::string data;
};
