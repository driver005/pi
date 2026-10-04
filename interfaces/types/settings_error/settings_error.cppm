export module pi.types.settings_error;

import std;

/** A problem reading or writing a settings file; the manager keeps running on defaults. */
export struct SettingsError {
    /** "global" or "project". */
    std::string scope;
    std::string path;
    std::string message;
};
