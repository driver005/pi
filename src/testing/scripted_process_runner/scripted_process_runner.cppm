export module pi.testing.scripted_process_runner;

import std;
export import pi.platform.i_process_runner;

/** IProcessRunner that answers from a handler and records every request. */
export class ScriptedProcessRunner : public IProcessRunner {
public:
    using Handler = std::function<Result<ProcessResult>(const ProcessRequest&)>;

    explicit ScriptedProcessRunner(Handler handler);

    int calls() const;
    std::vector<ProcessRequest> requests() const;

    Result<ProcessResult> run(const ProcessRequest& request) override;

private:
    Handler m_handler;
    mutable std::mutex m_mutex;
    std::vector<ProcessRequest> m_requests;
};

ScriptedProcessRunner::ScriptedProcessRunner(Handler handler) : m_handler(std::move(handler)) {}

int ScriptedProcessRunner::calls() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_requests.size());
}

std::vector<ProcessRequest> ScriptedProcessRunner::requests() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
}

Result<ProcessResult> ScriptedProcessRunner::run(const ProcessRequest& request) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_requests.push_back(request);
    }
    return m_handler(request);
}
