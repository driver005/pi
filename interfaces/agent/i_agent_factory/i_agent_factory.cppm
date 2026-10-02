export module pi.agent.i_agent_factory;

import std;
export import pi.agent.i_agent;
export import pi.types.agent_options;

/** Builds agents; lets a session create its agent (with hooks that refer to the session). */
export class IAgentFactory {
public:
    virtual ~IAgentFactory() = default;

    virtual std::unique_ptr<IAgent> create(AgentOptions options) = 0;
};
