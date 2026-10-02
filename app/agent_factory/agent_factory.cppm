export module pi.agent_factory;

import std;
export import pi.agent.i_agent_factory;
export import pi.agent.i_agent_loop;
export import pi.platform.i_clock;
import pi.agent.agent;

/** IAgentFactory creating Agent instances that share one loop and clock. */
export class AgentFactory : public IAgentFactory {
public:
    AgentFactory(IAgentLoop& loop, const IClock& clock);

    std::unique_ptr<IAgent> create(AgentOptions options) override;

private:
    IAgentLoop& m_loop;
    const IClock& m_clock;
};

AgentFactory::AgentFactory(IAgentLoop& loop, const IClock& clock) : m_loop(loop), m_clock(clock) {}

std::unique_ptr<IAgent> AgentFactory::create(AgentOptions options) {
    return std::make_unique<Agent>(std::move(options), m_loop, m_clock);
}
