export module pi.support.resolved_agent;

import std;
export import pi.durable.i_agent;
export import pi.types.extension;

/** An agent resolved from stored state, a registry snapshot and the settings; fixed for one task phase. */
export class ResolvedAgent : public IAgent {
public:
    ResolvedAgent(std::shared_ptr<const AgentSnapshot> snapshot, std::vector<PromptSection> sections,
                  std::vector<std::shared_ptr<const Extension>> extensions)
        : m_snapshot(std::move(snapshot)), m_sections(std::move(sections)), m_extensions(std::move(extensions)) {}

    std::shared_ptr<const AgentSnapshot> snapshot() const override {
        return m_snapshot;
    }

    const std::vector<PromptSection>& sections() const override {
        return m_sections;
    }

    std::vector<HookRegistration> hooks(const std::string& taskName) const override {
        std::vector<HookRegistration> found;
        for (const auto& extension : m_extensions) {
            for (const HookRegistration& registration : extension->hooks) {
                if (registration.task == taskName) {
                    found.push_back(registration);
                }
            }
        }
        return found;
    }

private:
    std::shared_ptr<const AgentSnapshot> m_snapshot;
    std::vector<PromptSection> m_sections;
    std::vector<std::shared_ptr<const Extension>> m_extensions;
};
