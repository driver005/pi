export module pi.support.tool_set_cache;

import std;
export import pi.tool.i_tool;

/**
 * The tools of one working directory, built on first use and kept: a durable conversation runs its tools in its own
 * directory, while the coding tools are bound to the directory they were built for.
 */
export class ToolSetCache {
public:
    using ToolSet = std::vector<std::shared_ptr<ITool>>;
    using Factory = std::function<ToolSet(const std::string& cwd)>;

    explicit ToolSetCache(Factory factory)
        : m_factory(std::move(factory)) {}

    ToolSetCache(const ToolSetCache&) = delete;
    ToolSetCache& operator=(const ToolSetCache&) = delete;

    ToolSet tools(const std::string& cwd) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_sets.find(cwd);
        if (found == m_sets.end()) {
            found = m_sets.emplace(cwd, m_factory(cwd)).first;
        }
        return found->second;
    }

    /** The tool called `name` in `cwd`; null when that directory's set has none. */
    std::shared_ptr<ITool> find(const std::string& cwd, const std::string& name) {
        for (const std::shared_ptr<ITool>& tool : tools(cwd)) {
            if (tool->definition().name == name) {
                return tool;
            }
        }
        return nullptr;
    }

private:
    Factory m_factory;
    std::mutex m_mutex;
    std::map<std::string, ToolSet> m_sets;
};
