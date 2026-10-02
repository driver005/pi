export module pi.types.plugin_context;

import std;

/** What a plugin may ask the host about the session it runs in. */
export struct PluginContext {
    std::string cwd;
    std::string agentDir;
};
