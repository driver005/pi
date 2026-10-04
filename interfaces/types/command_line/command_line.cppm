export module pi.types.command_line;

import std;
export import pi.types.coding_application_options;

/** A parsed command line: what to run and how the application is configured for it. */
export struct CommandLine {
    /** "rpc", "serve" or "mcp"; empty when only help was requested. */
    std::string command;
    /** Positional arguments after the command: `mcp login|logout <server>` or `mcp list`. */
    std::vector<std::string> arguments;
    bool help = false;
    CodingApplicationOptions options;
    /** `serve`: the profile and socket directory ($PI_SERVER_DIR, else ~/.pi/server). */
    std::string serverDir;
    /** `serve`: the logical server id (--server-id, else $PI_SERVER_ID); the directory's default when absent. */
    std::optional<std::string> serverId;
    /** `serve`: keep sessions as session trees over the AgentSession runtime instead of durable harness sessions. */
    bool sessionTree = false;
};
