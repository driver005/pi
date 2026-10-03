export module pi.types.tool_diagnostic;

import std;

/** A remark about one tool call for the model and the UI, such as truncation or a spill path; never part of the tool's data. */
export struct ToolDiagnostic {
    /** "info", "warn" or "error". */
    std::string severity = "info";
    std::string message;
    std::optional<std::string> code;
};
