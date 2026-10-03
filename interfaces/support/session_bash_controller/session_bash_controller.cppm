export module pi.support.session_bash_controller;

import std;
export import pi.agent.i_agent;
export import pi.platform.i_clock;
export import pi.session.i_session_event_sink;
export import pi.session.i_session_manager;
export import pi.session.i_settings_manager;
export import pi.support.bash_command_executor;
export import pi.support.session_context_refresher;
export import pi.types.bash_result;

/**
 * Runs commands the user types (the `!` prefix) and records them in the session so the model can
 * see their output. While the agent is running, results wait until the run ends so they do not
 * land between a tool call and its result. Port of the bash section of AgentSession.
 */
export class SessionBashController {
public:
    SessionBashController(IAgent& agent, ISessionManager& session, ISettingsManager& settings, SessionContextRefresher& refresher, ISessionEventSink& sink, BashCommandExecutor& executor, const IClock& clock)
        : m_agent(agent),
          m_session(session),
          m_settings(settings),
          m_refresher(refresher),
          m_sink(sink),
          m_executor(executor),
          m_clock(clock) {}

    /** excludeFromContext keeps the output away from the model (the `!!` prefix). */
    Result<BashResult> execute(const std::string& command, const std::function<void(const std::string&)>& onChunk, bool excludeFromContext, const std::optional<std::string>& id) {
        const auto signal = std::make_shared<AbortSignal>();
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_running.insert(signal);
        }
        const SettingsView settings = m_settings.view();
        const std::string prefix = settings.shellCommandPrefix().value_or("");
        const std::string resolved = prefix.empty() ? command : prefix + "\n" + command;
        const auto result = m_executor.execute(
            resolved, m_session.cwd(), settings.shellPath().value_or(""),
            [&](const std::string& delta) {
                if (onChunk) {
                    onChunk(delta);
                }
                AgentSessionEvent event;
                event.type = SessionEventType::BashExecutionUpdate;
                event.id = id;
                event.delta = delta;
                m_sink.emit(event);
            },
            signal);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_running.erase(signal);
        }
        if (result) {
            record(command, *result, excludeFromContext);
        }
        return result;
    }

    /** Records a result the caller obtained itself (plugins that run commands elsewhere). */
    void record(const std::string& command, const BashResult& result, bool excludeFromContext) {
        const CustomMessage message = toMessage(command, result, excludeFromContext);
        if (m_agent.isRunning()) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.push_back(message);
            return;
        }
        m_session.appendMessage(message);
        m_refresher.refresh();
    }

    /** Appends results that waited for the agent run to end. */
    void flushPending() {
        std::vector<CustomMessage> pending;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            pending.swap(m_pending);
        }
        if (pending.empty()) {
            return;
        }
        for (const auto& message : pending) {
            m_session.appendMessage(message);
        }
        m_refresher.refresh();
    }

    void abort() {
        std::vector<std::shared_ptr<AbortSignal>> signals;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            signals.assign(m_running.begin(), m_running.end());
        }
        for (const auto& signal : signals) {
            signal->abort();
        }
    }

    bool running() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return !m_running.empty();
    }

    bool hasPending() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return !m_pending.empty();
    }

private:
    CustomMessage toMessage(const std::string& command, const BashResult& result, bool excludeFromContext) const {
        CustomMessage message;
        message.role = "bashExecution";
        message.timestamp = m_clock.nowMs();
        message.data = Json::object();
        message.data["command"] = command;
        message.data["output"] = result.output;
        if (result.exitCode) {
            message.data["exitCode"] = *result.exitCode;
        }
        message.data["cancelled"] = result.cancelled;
        message.data["truncated"] = result.truncated;
        if (result.fullOutputPath) {
            message.data["fullOutputPath"] = *result.fullOutputPath;
        }
        if (excludeFromContext) {
            message.data["excludeFromContext"] = true;
        }
        return message;
    }

    IAgent& m_agent;
    ISessionManager& m_session;
    ISettingsManager& m_settings;
    SessionContextRefresher& m_refresher;
    ISessionEventSink& m_sink;
    BashCommandExecutor& m_executor;
    const IClock& m_clock;

    mutable std::mutex m_mutex;
    std::set<std::shared_ptr<AbortSignal>> m_running;
    std::vector<CustomMessage> m_pending;
};
