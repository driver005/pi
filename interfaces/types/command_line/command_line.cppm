export module pi.types.command_line;

import std;
export import pi.types.coding_application_options;

/** A parsed command line: what to run and how the application is configured for it. */
export struct CommandLine {
    /** "rpc" or "serve"; empty when only help was requested. */
    std::string command;
    bool help = false;
    CodingApplicationOptions options;
    /** `serve`: the profile and socket directory ($PI_SERVER_DIR, else ~/.pi/server). */
    std::string serverDir;
    /** `serve`: the logical server id (--server-id, else $PI_SERVER_ID); the directory's default when absent. */
    std::optional<std::string> serverId;
};
