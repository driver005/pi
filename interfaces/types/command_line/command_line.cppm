module;

#include <cstdint>

export module pi.types.command_line;

import std;
export import pi.types.coding_application_options;

/** A parsed command line: what to run and how the application is configured for it. */
export struct CommandLine {
    /** "rpc", "serve", "mcp", "auth", "export", "evals", "install", "remove", "update" or "list"; empty when only help was requested. */
    std::string command;
    /** Positional arguments after the command: `mcp login|logout <server>`, `mcp list`, `auth login|logout <provider>`, `auth list|status`, `install|remove <source>`, `update [source]`, `export <session> [output]`, `evals plan <discovered.json>|report <dir>|observe <task.json> <report.json>`. */
    std::vector<std::string> arguments;
    bool help = false;
    CodingApplicationOptions options;
    /** `serve`: the profile and socket directory ($PI_SERVER_DIR, else ~/.pi/server). */
    std::string serverDir;
    /** `serve`: the logical server id (--server-id, else $PI_SERVER_ID); the directory's default when absent. */
    std::optional<std::string> serverId;
    /** `serve`: keep sessions as session trees over the AgentSession runtime instead of durable harness sessions. */
    bool sessionTree = false;
    /** `auth login`: the sign-in method (--method), empty for the provider's default. */
    std::string loginMethod;
    /** `auth login`: ask for the code instead of using the loopback callback (--manual). */
    bool loginManual = false;
    /** `evals plan`: repetitions per documentation variant (--runs). */
    std::int64_t evalRuns = 1;
    /** `export`: the theme of the page (--theme), empty for the default. */
    std::string exportTheme;
    /** `install`, `remove`: the project's settings instead of the global ones (-l, --local). */
    bool localPackages = false;
};
