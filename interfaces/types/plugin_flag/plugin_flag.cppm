export module pi.types.plugin_flag;

import std;
export import pi.types.json;

/** A command line flag a plugin declared. */
export struct PluginFlag {
    std::string name;
    std::string description;
    /** "boolean" or "string". */
    std::string type = "boolean";
    /** The default value; null when there is none. */
    Json defaultValue;
    /** The plugin library that declared it. */
    std::string path;
};
