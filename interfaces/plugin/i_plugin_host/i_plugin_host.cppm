export module pi.plugin.i_plugin_host;

import std;
export import pi.types.error;

/**
 * Loads plugin libraries and connects them to the session: their tools go into the tool registry
 * and their hook subscriptions into the hook bus. A plugin that fails to load does not stop the
 * others.
 */
export class IPluginHost {
public:
    virtual ~IPluginHost() = default;

    /** Loads the libraries in order; returns one Error per plugin that could not be loaded. */
    virtual std::vector<Error> load(const std::vector<std::string>& paths) = 0;
    /** Paths of the plugins that are loaded. */
    virtual std::vector<std::string> loaded() const = 0;
    /** Unloads every plugin (reverse order): shutdown callback, tools and hooks removed. Idempotent. */
    virtual void shutdown() = 0;
};
