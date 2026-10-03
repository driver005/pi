export module pi.support.registry_state;

import std;
export import pi.durable.i_registry_snapshot;
export import pi.types.result;

/** One immutable published registry state: the installed extensions and the task definitions they bring. */
export class RegistryState : public IRegistrySnapshot {
public:
    /** Builds the state; fails when two task definitions share a name. Call once, before sharing. */
    Result<void> assign(std::vector<std::shared_ptr<const Extension>> extensions,
                        const std::vector<std::shared_ptr<const TaskDefinition>>& builtins) {
        m_extensions = std::move(extensions);
        m_byName.clear();
        m_tasks.clear();
        for (const auto& task : builtins) {
            m_tasks[task->name] = task;
        }
        for (const auto& extension : m_extensions) {
            m_byName[extension->name] = extension;
            for (const auto& task : extension->tasks) {
                if (m_tasks.contains(task->name)) {
                    return std::unexpected(Error{"registry_error", "Task " + task->name + " of extension " +
                                                                       extension->name + " is already installed"});
                }
                m_tasks[task->name] = task;
            }
        }
        return {};
    }

    std::vector<std::shared_ptr<const Extension>> installed() const override {
        return m_extensions;
    }

    std::shared_ptr<const Extension> extension(const std::string& name) const override {
        auto found = m_byName.find(name);
        return found == m_byName.end() ? nullptr : found->second;
    }

    std::vector<std::shared_ptr<const TaskDefinition>> tasks() const override {
        std::vector<std::shared_ptr<const TaskDefinition>> all;
        for (const auto& item : m_tasks) {
            all.push_back(item.second);
        }
        return all;
    }

    std::shared_ptr<const TaskDefinition> task(const std::string& name) const override {
        auto found = m_tasks.find(name);
        return found == m_tasks.end() ? nullptr : found->second;
    }

private:
    std::vector<std::shared_ptr<const Extension>> m_extensions;
    std::map<std::string, std::shared_ptr<const Extension>> m_byName;
    std::map<std::string, std::shared_ptr<const TaskDefinition>> m_tasks;
};
