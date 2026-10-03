export module pi.support.tool_set_cache;

import std;
export import pi.tool.i_tool;

/**
 * The tools of one working directory, built on first use and kept: a durable conversation runs its tools in its own
 * directory, while the coding tools are bound to the directory they were built for. The set of a directory can be dropped
 * (tools of plugins and MCP servers appear while a session runs); listeners are told, and the next use builds it again. The
 * factory runs outside the cache's lock, so it may take as long as it needs and may call back into the cache.
 */
export class ToolSetCache {
public:
    using ToolSet = std::vector<std::shared_ptr<ITool>>;
    using Factory = std::function<ToolSet(const std::string& cwd)>;
    /** Told which directory's set was dropped. */
    using ChangeListener = std::function<void(const std::string& cwd)>;

    explicit ToolSetCache(Factory factory)
        : m_factory(std::move(factory)) {}

    ToolSetCache(const ToolSetCache&) = delete;
    ToolSetCache& operator=(const ToolSetCache&) = delete;

    ToolSet tools(const std::string& cwd) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_sets.find(cwd);
            if (found != m_sets.end()) {
                return found->second;
            }
        }
        ToolSet built = m_factory(cwd);
        // A set built concurrently by another caller wins, so every caller sees the same tool objects.
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_sets.emplace(cwd, std::move(built)).first->second;
    }

    /** Drops the set of `cwd`, so the next use builds it again, and tells the listeners. */
    void invalidate(const std::string& cwd) {
        std::vector<ChangeListener> listeners;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_sets.erase(cwd);
            for (const auto& entry : m_listeners) {
                listeners.push_back(entry.second);
            }
        }
        for (const ChangeListener& listener : listeners) {
            listener(cwd);
        }
    }

    std::int64_t subscribe(ChangeListener listener) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.emplace(++m_nextListener, std::move(listener));
        return m_nextListener;
    }

    void unsubscribe(std::int64_t id) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.erase(id);
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
    std::int64_t m_nextListener = 0;
    std::map<std::int64_t, ChangeListener> m_listeners;
};
