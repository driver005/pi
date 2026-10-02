export module pi.types.auth_result;

import std;
export import pi.types.model_auth;

/** Resolved auth plus provider-scoped environment values and a label for status displays. */
export struct AuthResult {
    ModelAuth auth;
    std::map<std::string, std::string> env;
    /** "ANTHROPIC_API_KEY", "OAuth", "stored credential", ... */
    std::string source;
};
