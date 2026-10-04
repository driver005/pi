export module pi.tools.tool_registry;

import std;
export import pi.tool.i_tool_registry;

/** Mutex-guarded IToolRegistry (parallel vectors keep registration order). */
export class ToolRegistry : public IToolRegistry {
public:
    void add(std::shared_ptr<ITool> tool) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto index = indexOf(tool->definition().name);
        if (index.has_value()) {
            m_tools[*index] = std::move(tool);
            return;
        }
        m_tools.push_back(std::move(tool));
        m_active.push_back(true);
    }

    bool remove(const std::string& name) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto index = indexOf(name);
        if (!index.has_value()) {
            return false;
        }
        m_tools.erase(m_tools.begin() + static_cast<std::ptrdiff_t>(*index));
        m_active.erase(m_active.begin() + static_cast<std::ptrdiff_t>(*index));
        return true;
    }

    std::shared_ptr<ITool> find(const std::string& name) const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto index = indexOf(name);
        return index.has_value() ? m_tools[*index] : nullptr;
    }

    std::vector<std::shared_ptr<ITool>> all() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_tools;
    }

    std::vector<std::shared_ptr<ITool>> active() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::shared_ptr<ITool>> tools;
        for (std::size_t i = 0; i < m_tools.size(); ++i) {
            if (m_active[i]) {
                tools.push_back(m_tools[i]);
            }
        }
        return tools;
    }

    std::size_t setActive(const std::vector<std::string>& names) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        std::size_t count = 0;
        for (std::size_t i = 0; i < m_tools.size(); ++i) {
            const std::string& name = m_tools[i]->definition().name;
            m_active[i] = std::find(names.begin(), names.end(), name) != names.end();
            count += m_active[i] ? 1 : 0;
        }
        return count;
    }

private:
    std::optional<std::size_t> indexOf(const std::string& name) const {
        for (std::size_t i = 0; i < m_tools.size(); ++i) {
            if (m_tools[i]->definition().name == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    std::vector<std::shared_ptr<ITool>> m_tools;
    std::vector<bool> m_active;
    mutable std::mutex m_mutex;
};
