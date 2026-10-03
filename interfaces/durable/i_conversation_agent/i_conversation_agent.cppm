export module pi.durable.i_conversation_agent;

import std;
export import pi.types.agent_snapshot;
export import pi.types.hook_registration;
export import pi.types.prompt_section;

/** A conversation's agent resolved against a registry snapshot and the settings; fixed for one task phase. */
export class IConversationAgent {
public:
    virtual ~IConversationAgent() = default;

    virtual std::shared_ptr<const AgentSnapshot> snapshot() const = 0;
    /** Extension sections, then the instructions section when set. */
    virtual const std::vector<PromptSection>& sections() const = 0;
    /** The hook registrations of the selected extensions for a task name, in extension order. */
    virtual std::vector<HookRegistration> hooks(const std::string& taskName) const = 0;
};
