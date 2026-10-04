export module pi.support.agent_resolver;

import std;
export import pi.durable.i_registry_snapshot;
export import pi.support.registry;
export import pi.support.resolved_agent;
export import pi.types.error;
export import pi.types.json;
export import pi.types.resolved_settings;

/**
 * Resolves a conversation's agent from its stored state (absent: every field unset), a registry snapshot and the
 * resolved settings. A wrapper that renames its target drops it and is reported; a wrapper without a target does
 * nothing. Port of resolveAgent() in packages/durable/src/harness/agent.ts.
 */
export class AgentResolver {
public:
    std::shared_ptr<ResolvedAgent> resolve(const std::optional<Json>& state, const IRegistrySnapshot& snapshot,
                                           const ResolvedSettings& settings,
                                           const std::function<void(const Error&)>& report) const {
        const Json none = Json::object();
        const Json& stored = state ? *state : none;
        const std::vector<std::shared_ptr<const Extension>> extensions =
            selectExtensions(stored.contains("extensions") ? &stored.at("extensions") : nullptr, snapshot, settings);

        std::vector<std::string> toolOrder;
        std::map<std::string, ToolRegistration> composed;
        std::vector<std::string> sectionOrder;
        std::map<std::string, PromptSection> sections;
        for (const auto& extension : extensions) {
            for (const ToolRegistration& tool : extension->tools) {
                if (!composed.contains(tool.name)) {
                    toolOrder.push_back(tool.name);
                }
                composed[tool.name] = tool;
            }
            for (const PromptSection& section : extension->sections) {
                if (!sections.contains(section.key)) {
                    sectionOrder.push_back(section.key);
                }
                sections[section.key] = section;
            }
        }
        for (const auto& extension : extensions) {
            for (const Wrap& wrap : extension->wraps) {
                if (wrap.kind == "tool") {
                    applyWrap(composed, toolOrder, wrap.target, wrap.wrapTool, report);
                } else {
                    applyWrap(sections, sectionOrder, wrap.target, wrap.wrapSection, report);
                }
            }
        }

        auto snapshotOf = std::make_shared<AgentSnapshot>();
        for (const std::string& name : filterTools(stored, toolOrder)) {
            snapshotOf->tools.push_back(composed.at(name));
        }
        std::vector<PromptSection> agentSections;
        for (const std::string& key : sectionOrder) {
            agentSections.push_back(sections.at(key));
        }
        if (stored.contains("instructions")) {
            const std::string instructions = stored.at("instructions").get<std::string>();
            snapshotOf->instructions = instructions;
            agentSections.push_back(PromptSection{std::string(Registry::kInstructionsKey),
                                                  [instructions](const PromptInput&) -> Result<std::optional<std::string>> {
                                                      return std::optional<std::string>(instructions);
                                                  },
                                                  true});
        }
        if (stored.contains("model")) {
            snapshotOf->model = stored.at("model");
        }
        snapshotOf->thinkingLevel = stored.value("thinkingLevel", std::string("off"));
        if (stored.contains("cwd")) {
            snapshotOf->cwd = stored.at("cwd").get<std::string>();
        }
        for (const auto& extension : extensions) {
            snapshotOf->extensionNames.push_back(extension->name);
        }
        return std::make_shared<ResolvedAgent>(snapshotOf, std::move(agentSections), extensions);
    }

private:
    /** The selected installed extensions: the stored list, or the default selection edited by `{add, remove}`. */
    std::vector<std::shared_ptr<const Extension>> selectExtensions(const Json* stored, const IRegistrySnapshot& snapshot,
                                                                   const ResolvedSettings& settings) const {
        std::vector<std::string> selected;
        if (stored != nullptr && stored->is_array()) {
            for (const Json& name : *stored) {
                selected.push_back(name.get<std::string>());
            }
        } else {
            if (settings.extensions) {
                selected = *settings.extensions;
            } else {
                for (const auto& installed : snapshot.installed()) {
                    selected.push_back(installed->name);
                }
            }
            std::set<std::string> removed;
            if (stored != nullptr && stored->contains("remove")) {
                for (const Json& name : stored->at("remove")) {
                    removed.insert(name.get<std::string>());
                }
            }
            if (stored != nullptr && stored->contains("add")) {
                for (const Json& name : stored->at("add")) {
                    selected.push_back(name.get<std::string>());
                }
            }
            std::erase_if(selected, [&](const std::string& name) { return removed.contains(name); });
        }
        std::vector<std::shared_ptr<const Extension>> extensions;
        std::set<std::string> seen;
        for (const std::string& name : selected) {
            if (!seen.insert(name).second) {
                continue;
            }
            if (auto extension = snapshot.extension(name)) {
                extensions.push_back(extension);
            }
        }
        return extensions;
    }

    template <typename Item, typename Wrapper>
    void applyWrap(std::map<std::string, Item>& items, std::vector<std::string>& order, const std::string& target,
                   const Wrapper& wrapper, const std::function<void(const Error&)>& report) const {
        auto found = items.find(target);
        if (found == items.end() || !wrapper) {
            return;
        }
        Item wrapped = wrapper(found->second);
        const std::string name = nameOf(wrapped);
        if (name != target) {
            items.erase(found);
            std::erase(order, target);
            report(Error{"wrapper_error", "Wrapper renamed " + target + " to " + name});
            return;
        }
        found->second = std::move(wrapped);
    }

    std::string nameOf(const ToolRegistration& tool) const {
        return tool.name;
    }

    std::string nameOf(const PromptSection& section) const {
        return section.key;
    }

    /** The tools a request offers: every composed tool, exactly the listed ones in order, or all but the removed. */
    std::vector<std::string> filterTools(const Json& stored, const std::vector<std::string>& order) const {
        if (!stored.contains("tools")) {
            return order;
        }
        const Json& filter = stored.at("tools");
        std::vector<std::string> names;
        if (filter.is_array()) {
            std::set<std::string> seen;
            for (const Json& item : filter) {
                const std::string name = item.get<std::string>();
                if (seen.insert(name).second && std::find(order.begin(), order.end(), name) != order.end()) {
                    names.push_back(name);
                }
            }
            return names;
        }
        std::set<std::string> removed;
        for (const Json& item : filter.at("remove")) {
            removed.insert(item.get<std::string>());
        }
        for (const std::string& name : order) {
            if (!removed.contains(name)) {
                names.push_back(name);
            }
        }
        return names;
    }
};
