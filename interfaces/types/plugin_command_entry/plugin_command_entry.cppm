module;

#include "pi_plugin.h"

export module pi.types.plugin_command_entry;

import std;

/** A registered plugin command with the function that runs it. */
export struct PluginCommandEntry {
    std::string name;
    std::string description;
    std::string path;
    PiCommandFn handler = nullptr;
    void* userData = nullptr;
};
