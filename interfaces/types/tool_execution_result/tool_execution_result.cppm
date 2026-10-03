export module pi.types.tool_execution_result;

import std;
export import pi.types.json;
export import pi.types.tool_diagnostic;

/**
 * What a tool execution returns. Absent `content` takes the retained running output, absent `details` the
 * last reported details; `diagnostics` follow those reported while running.
 */
export struct ToolExecutionResult {
    /** A JSON array of text/image content blocks. */
    std::optional<Json> content;
    std::optional<bool> isError;
    std::optional<Json> details;
    std::vector<ToolDiagnostic> diagnostics;
    /** Spend of the execution itself, such as a model call. */
    std::optional<Json> usage;
    /** `{addTools?: [names], terminate?: true, handoff?: string}`. */
    std::optional<Json> control;
};
