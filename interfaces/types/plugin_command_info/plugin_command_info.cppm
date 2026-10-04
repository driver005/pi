export module pi.types.plugin_command_info;

import std;

/** A command a plugin registered. */
export struct PluginCommandInfo {
    std::string name;
    std::string description;
    /** The plugin library that registered it. */
    std::string path;
};
