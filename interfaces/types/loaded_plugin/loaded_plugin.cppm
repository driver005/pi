module;

#include <cstdint>

#include "pi_plugin.h"

export module pi.types.loaded_plugin;

import std;

/** A plugin the host loaded: where it came from, what it registered and the API it was given. */
export struct LoadedPlugin {
    std::string path;
    /** File name without directory and extension, for log messages. */
    std::string name;
    std::uint64_t library = 0;
    PiPluginShutdownFn shutdown = nullptr;
    /** The host that loaded it (opaque to this type). */
    void* owner = nullptr;
    PiHostApi api{};
    std::vector<std::string> tools;
    std::vector<std::uint64_t> subscriptions;
    std::vector<std::string> providers;
    std::vector<std::string> mcpServers;
    /** Virtual models (provider, id). */
    std::vector<std::pair<std::string, std::string>> virtualModels;
};
