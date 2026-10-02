export module pi.types.auth_status;

import std;

/** Whether a provider has usable credentials, and where they come from. */
export struct AuthStatus {
    bool configured = false;
    /** "stored" | "runtime" | "environment" | "models_json_key" | "models_json_command" | "ambient". */
    std::string source;
    /** Variable names for the environment source. */
    std::string label;
};
