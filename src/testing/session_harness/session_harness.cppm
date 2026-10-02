export module pi.testing.session_harness;

import std;
import pi.agent.agent_loop;
import pi.agent.tool_call_runner;
import pi.agent_factory;
import pi.ai.faux_provider;
import pi.session.session_manager;
export import pi.session.i_session_manager;
export import pi.agent.i_agent_factory;
export import pi.agent.i_agent;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.recording_sleeper;
import pi.testing.sequential_id_generator;

/**
 * Everything a session test needs, wired in memory: a faux provider behind a real agent loop,
 * an in-memory session tree, a recording sleeper and fixed clock. Tests script the provider and
 * inspect the session; nothing touches the network or disk.
 */
export class SessionHarness {
public:
    explicit SessionHarness(std::string cwd = "/work");

    FauxProvider& provider();
    FixedClock& clock();
    FakeFileSystem& files();
    RecordingSleeper& sleeper();
    SequentialIdGenerator& ids();
    ISessionManager& session();
    IAgentFactory& agents();

    /** Agent options with the faux model and a stream function bound to the provider. */
    AgentOptions agentOptions();

    /** A real agent over the faux provider. */
    std::unique_ptr<IAgent> makeAgent();

private:
    InlineExecutor m_executor;
    FixedClock m_clock{1700000000000};
    FakeFileSystem m_files;
    RecordingSleeper m_sleeper;
    SequentialIdGenerator m_ids{"id"};
    FauxProvider m_provider{m_executor, m_clock};
    ToolCallRunner m_runner;
    AgentLoop m_loop{m_runner, m_executor, m_clock};
    AgentFactory m_agents{m_loop, m_clock};
    std::unique_ptr<SessionManager> m_session;
};

SessionHarness::SessionHarness(std::string cwd) {
    SessionManagerOptions options;
    options.cwd = std::move(cwd);
    options.persist = false;
    m_session = std::make_unique<SessionManager>(options, m_files, m_clock, m_ids);
    m_session->open();
}

FauxProvider& SessionHarness::provider() {
    return m_provider;
}

FixedClock& SessionHarness::clock() {
    return m_clock;
}

FakeFileSystem& SessionHarness::files() {
    return m_files;
}

RecordingSleeper& SessionHarness::sleeper() {
    return m_sleeper;
}

SequentialIdGenerator& SessionHarness::ids() {
    return m_ids;
}

ISessionManager& SessionHarness::session() {
    return *m_session;
}

IAgentFactory& SessionHarness::agents() {
    return m_agents;
}

AgentOptions SessionHarness::agentOptions() {
    AgentOptions options;
    options.model.id = "faux-1";
    options.model.api = "faux";
    options.model.provider = "faux";
    options.model.contextWindow = 100000;
    options.model.maxTokens = 8000;
    options.systemPrompt = "You are helpful";
    options.streamFn = [this](const Model& model, const TranscriptContext& context,
                              const StreamOptions& streamOptions) {
        return m_provider.stream(model, context, streamOptions);
    };
    return options;
}

std::unique_ptr<IAgent> SessionHarness::makeAgent() {
    return m_agents.create(agentOptions());
}
