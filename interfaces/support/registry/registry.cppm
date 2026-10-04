module;

#include <cstdint>

export module pi.support.registry;

import std;
export import pi.durable.i_registry_reader;
export import pi.support.registry_state;
export import pi.types.result;

/**
 * The application-owned registry of extensions: install, replace and remove them while a harness runs; every
 * change publishes a fresh immutable snapshot and calls the listeners synchronously. Port of registry.ts.
 */
export class Registry : public IRegistryReader {
public:
    static constexpr std::string_view kInstructionsKey = "instructions";

    /** `builtins` are the task definitions every registry holds; they cannot be removed or replaced. */
    explicit Registry(std::vector<std::shared_ptr<const TaskDefinition>> builtins = {}) : m_builtins(std::move(builtins)) {
        auto initial = std::make_shared<RegistryState>();
        (void)initial->assign({}, m_builtins);
        m_current = initial;
    }

    std::shared_ptr<const IRegistrySnapshot> snapshot() const override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_current;
    }

    std::int64_t subscribe(std::function<void()> listener) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const std::int64_t handle = ++m_nextListener;
        m_listeners[handle] = std::move(listener);
        return handle;
    }

    void unsubscribe(std::int64_t handle) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.erase(handle);
    }

    /** Installs the extension, or replaces the installed extension with its name in place; publishes at once. */
    Result<void> install(Extension extension) {
        if (auto valid = validate(extension); !valid) {
            return valid;
        }
        auto added = std::make_shared<const Extension>(std::move(extension));
        std::vector<std::shared_ptr<const Extension>> next = snapshot()->installed();
        bool replaced = false;
        for (auto& installed : next) {
            if (installed->name == added->name) {
                installed = added;
                replaced = true;
            }
        }
        if (!replaced) {
            next.push_back(added);
        }
        return publish(std::move(next));
    }

    /** Removes the installed extension with this name; a later install appends. */
    Result<void> uninstall(const std::string& name) {
        std::vector<std::shared_ptr<const Extension>> next = snapshot()->installed();
        const auto removed = std::erase_if(next, [&](const auto& installed) { return installed->name == name; });
        if (removed == 0) {
            return {};
        }
        return publish(std::move(next));
    }

private:
    /** Unique tool names and section keys within one extension; valid, unreserved section keys. */
    Result<void> validate(const Extension& extension) const {
        std::set<std::string> tools;
        for (const ToolRegistration& tool : extension.tools) {
            if (!tools.insert(tool.name).second) {
                return std::unexpected(Error{"registry_error", "Extension " + extension.name + " has two tools named " + tool.name});
            }
        }
        std::set<std::string> sections;
        for (const PromptSection& section : extension.sections) {
            if (!validKey(section.key)) {
                return std::unexpected(Error{"registry_error", "Section key \"" + section.key + "\" must match ^[a-z][a-z0-9_-]*$"});
            }
            if (section.key == kInstructionsKey) {
                return std::unexpected(Error{"registry_error", "Section key " + section.key + " is reserved for the agent's instructions"});
            }
            if (!sections.insert(section.key).second) {
                return std::unexpected(Error{"registry_error", "Extension " + extension.name + " has two sections with key " + section.key});
            }
        }
        return {};
    }

    bool validKey(const std::string& key) const {
        if (key.empty() || key[0] < 'a' || key[0] > 'z') {
            return false;
        }
        return std::ranges::all_of(key, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
    }

    /** Builds and validates the next state, then publishes it and calls the listeners synchronously. */
    Result<void> publish(std::vector<std::shared_ptr<const Extension>> extensions) {
        auto state = std::make_shared<RegistryState>();
        if (auto built = state->assign(std::move(extensions), m_builtins); !built) {
            return built;
        }
        std::vector<std::function<void()>> listeners;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_current = state;
            for (const auto& item : m_listeners) {
                listeners.push_back(item.second);
            }
        }
        for (const auto& listener : listeners) {
            listener();
        }
        return {};
    }

    std::vector<std::shared_ptr<const TaskDefinition>> m_builtins;
    mutable std::mutex m_mutex;
    std::shared_ptr<const RegistryState> m_current;
    std::int64_t m_nextListener = 0;
    std::map<std::int64_t, std::function<void()>> m_listeners;
};
