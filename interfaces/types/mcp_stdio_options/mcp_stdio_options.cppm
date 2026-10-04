export module pi.types.mcp_stdio_options;

import std;

/** How to start a stdio MCP server. */
export struct McpStdioOptions {
    std::string command;
    std::vector<std::string> args;
    std::optional<std::string> cwd;
    /** Layered over the parent's environment. */
    std::map<std::string, std::string> env;
    /** After stdin closes, the server gets this long (at most 500 ms of it before SIGTERM) to exit, then SIGKILL. */
    std::int64_t closeTimeoutMs = 2000;
    std::size_t maxMessageBytes = 16 * 1024 * 1024;
    std::size_t maxStderrBytes = 64 * 1024;
};
