export module pi.types.config_value_part;

import std;

/** One piece of a config value template: literal text or an environment variable reference. */
export struct ConfigValuePart {
    bool isEnv = false;
    /** Literal text, or the variable name when isEnv. */
    std::string value;
};
