export module pi.coding_services;

import std;
export import pi.model_services;
export import pi.platform_services;
import pi.agent.agent_loop;
import pi.agent.tool_call_runner;
import pi.agent_factory;
import pi.session.project_trust_store;
import pi.session.session_store;
import pi.session_manager_factory;
import pi.support.project_trust_probe;
import pi.support.project_trust_resolver;

/**
 * The process-wide services behind every coding session: the operating-system services, the
 * model runtime, the shared agent loop and the stores that live in the agent directory.
 */
export class CodingServices {
public:
    CodingServices(const std::string& agentDir, const std::string& catalogDir, bool faux);

    PlatformServices& platform();
    ModelServices& models();
    IAgentFactory& agents();
    ISessionStore& sessions();
    ProjectTrustResolver& trust();

private:
    PlatformServices m_platform;
    ModelServices m_models;
    ToolCallRunner m_toolCalls;
    AgentLoop m_loop;
    AgentFactory m_agents;
    SessionManagerFactory m_managers;
    SessionStore m_sessions;
    ProjectTrustStore m_trustStore;
    ProjectTrustProbe m_trustProbe;
    ProjectTrustResolver m_trust;
};

CodingServices::CodingServices(const std::string& agentDir, const std::string& catalogDir, bool faux)
    : m_platform(),
      m_models(m_platform, agentDir, catalogDir, faux),
      m_loop(m_toolCalls, m_platform.executor(), m_platform.clock()),
      m_agents(m_loop, m_platform.clock()),
      m_managers(m_platform.files(), m_platform.clock(), m_platform.ids()),
      m_sessions(agentDir, m_platform.files(), m_platform.clock(), m_platform.ids(), m_managers),
      m_trustStore(agentDir, m_platform.files(), m_platform.locks()),
      m_trustProbe(m_platform.files()),
      m_trust(m_trustStore, m_trustProbe) {}

PlatformServices& CodingServices::platform() {
    return m_platform;
}

ModelServices& CodingServices::models() {
    return m_models;
}

IAgentFactory& CodingServices::agents() {
    return m_agents;
}

ISessionStore& CodingServices::sessions() {
    return m_sessions;
}

ProjectTrustResolver& CodingServices::trust() {
    return m_trust;
}
