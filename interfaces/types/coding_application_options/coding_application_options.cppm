export module pi.types.coding_application_options;

import std;
export import pi.types.coding_startup_options;
export import pi.types.session_start_mode;

/** Everything the coding application is configured with at process start. */
export struct CodingApplicationOptions {
    std::string cwd;
    std::string agentDir;
    /** Provider and model catalog directory; empty means <agentDir>/catalog. */
    std::string catalogDir;
    /** Adds the scripted offline provider "faux" (replies from PI_FAUX_REPLIES). */
    bool faux = false;
    CodingStartupOptions startup;
    SessionStartMode sessionMode = SessionStartMode::New;
    /** Session file path or id for SessionStartMode::Open. */
    std::optional<std::string> sessionRef;
    std::optional<std::string> sessionDir;
};
