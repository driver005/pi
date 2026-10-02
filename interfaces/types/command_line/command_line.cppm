export module pi.types.command_line;

import std;
export import pi.types.coding_application_options;

/** A parsed command line: what to run and how the application is configured for it. */
export struct CommandLine {
    /** "rpc"; empty when only help was requested. */
    std::string command;
    bool help = false;
    CodingApplicationOptions options;
};
