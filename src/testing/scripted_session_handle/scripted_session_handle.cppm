export module pi.testing.scripted_session_handle;

import std;
export import pi.server.i_routed_session_handle;
export import pi.testing.scripted_session_state;

/** IRoutedSessionHandle driven by a ScriptedSessionState. */
export class ScriptedSessionHandle : public IRoutedSessionHandle {
public:
    explicit ScriptedSessionHandle(std::shared_ptr<ScriptedSessionState> state);

    Result<std::unique_ptr<IServiceAttachment>> attachClient(const ServiceContext& context) override;
    void onTermination(TerminationListener listener) override;
    Result<void> close(const ServiceContext& context) override;

private:
    std::shared_ptr<ScriptedSessionState> m_state;
};

ScriptedSessionHandle::ScriptedSessionHandle(std::shared_ptr<ScriptedSessionState> state) : m_state(std::move(state)) {}

Result<std::unique_ptr<IServiceAttachment>> ScriptedSessionHandle::attachClient(const ServiceContext&) {
    ++m_state->attached;
    std::shared_ptr<ScriptedSessionState> state = m_state;
    return std::unique_ptr<IServiceAttachment>(
        std::make_unique<ScriptedServiceAttachment>(state->handler, [state] { ++state->released; }));
}

void ScriptedSessionHandle::onTermination(TerminationListener listener) {
    const std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->listener = std::move(listener);
}

Result<void> ScriptedSessionHandle::close(const ServiceContext&) {
    ++m_state->closed;
    if (m_state->closeFailure) {
        return std::unexpected(*m_state->closeFailure);
    }
    return {};
}
