export module pi.testing.scripted_session_handle;

import std;
export import pi.server.i_routed_session_handle;
export import pi.testing.scripted_session_state;

/** IRoutedSessionHandle driven by a ScriptedSessionState. */
export class ScriptedSessionHandle : public IRoutedSessionHandle {
public:
    explicit ScriptedSessionHandle(std::shared_ptr<ScriptedSessionState> state)
        : m_state(std::move(state)) {}

    Result<std::unique_ptr<IServiceAttachment>> attachClient(const ServiceContext&) override {
        ++m_state->attached;
        std::shared_ptr<ScriptedSessionState> state = m_state;
        return std::unique_ptr<IServiceAttachment>(
            std::make_unique<ScriptedServiceAttachment>(state->handler, [state] { ++state->released; }));
    }

    void onTermination(TerminationListener listener) override {
        const std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->listener = std::move(listener);
    }

    Result<void> close(const ServiceContext&) override {
        ++m_state->closed;
        if (m_state->closeFailure) {
            return std::unexpected(*m_state->closeFailure);
        }
        return {};
    }

private:
    std::shared_ptr<ScriptedSessionState> m_state;
};
