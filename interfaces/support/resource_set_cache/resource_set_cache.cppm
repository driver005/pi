export module pi.support.resource_set_cache;

import std;
export import pi.types.loaded_resources;

/** The resources (context files, skills, SYSTEM.md) of one working directory, loaded on first use and kept, like pi at startup. */
export class ResourceSetCache {
public:
    using Factory = std::function<LoadedResources(const std::string& cwd)>;

    explicit ResourceSetCache(Factory factory)
        : m_factory(std::move(factory)) {}

    ResourceSetCache(const ResourceSetCache&) = delete;
    ResourceSetCache& operator=(const ResourceSetCache&) = delete;

    LoadedResources resources(const std::string& cwd) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_sets.find(cwd);
        if (found == m_sets.end()) {
            found = m_sets.emplace(cwd, m_factory(cwd)).first;
        }
        return found->second;
    }

private:
    Factory m_factory;
    std::mutex m_mutex;
    std::map<std::string, LoadedResources> m_sets;
};
