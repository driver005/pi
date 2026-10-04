export module pi.support.plugin_resource_loader;

import std;
export import pi.session.i_resource_loader;
export import pi.support.plugin_session_events;

/**
 * An IResourceLoader that lets plugins add resources before every load: it fires `resources_discover` ({cwd, reason: "startup" for
 * the first load, "reload" for the later ones}), hands the `skillPaths` and `promptPaths` the handlers answered to the wrapped
 * loader (each path once, however often it is answered) and then loads. Resources are read as the wrapped loader reads them.
 */
export class PluginResourceLoader : public IResourceLoader {
public:
    /** Gives the wrapped loader more skill and prompt template paths for its next load. */
    using AddPaths = std::function<void(const std::vector<std::string>& skills, const std::vector<std::string>& prompts)>;

    PluginResourceLoader(IResourceLoader& inner, PluginSessionEvents& events, std::string cwd, AddPaths addPaths)
        : m_inner(inner),
          m_events(events),
          m_cwd(std::move(cwd)),
          m_addPaths(std::move(addPaths)) {}

    LoadedResources resources() const override {
        return m_inner.resources();
    }

    Result<void> reload() override {
        std::string reason;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            reason = m_loaded ? "reload" : "startup";
            m_loaded = true;
        }
        const auto [skills, prompts] = m_events.resourcesDiscover(m_cwd, reason);
        const std::vector<std::string> newSkills = unseen(skills);
        const std::vector<std::string> newPrompts = unseen(prompts);
        if (m_addPaths && (!newSkills.empty() || !newPrompts.empty())) {
            m_addPaths(newSkills, newPrompts);
        }
        return m_inner.reload();
    }

private:
    std::vector<std::string> unseen(const std::vector<std::string>& paths) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> fresh;
        for (const std::string& path : paths) {
            if (m_known.insert(path).second) {
                fresh.push_back(path);
            }
        }
        return fresh;
    }

    IResourceLoader& m_inner;
    PluginSessionEvents& m_events;
    std::string m_cwd;
    AddPaths m_addPaths;
    std::mutex m_mutex;
    bool m_loaded = false;
    std::set<std::string> m_known;
};
