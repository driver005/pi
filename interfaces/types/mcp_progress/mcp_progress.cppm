export module pi.types.mcp_progress;

import std;

/** A notifications/progress report for a running request. */
export struct McpProgress {
    double progress = 0;
    std::optional<double> total;
    std::optional<std::string> message;
};
