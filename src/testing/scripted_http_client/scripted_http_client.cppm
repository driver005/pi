export module pi.testing.scripted_http_client;

import std;
export import pi.platform.i_http_client;

/** IHttpClient that replays queued results in order and records every request. */
export class ScriptedHttpClient : public IHttpClient {
public:
    void enqueue(Result<HttpResponse> result);
    int calls() const;
    std::vector<HttpRequest> requests() const;

    Result<HttpResponse> send(const HttpRequest& request) override;

private:
    mutable std::mutex m_mutex;
    std::vector<Result<HttpResponse>> m_script;
    std::vector<HttpRequest> m_requests;
};

void ScriptedHttpClient::enqueue(Result<HttpResponse> result) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_script.push_back(std::move(result));
}

int ScriptedHttpClient::calls() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_requests.size());
}

std::vector<HttpRequest> ScriptedHttpClient::requests() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
}

Result<HttpResponse> ScriptedHttpClient::send(const HttpRequest& request) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_requests.push_back(request);
    if (m_script.empty()) {
        return std::unexpected(Error{"transport", "script exhausted"});
    }
    auto next = std::move(m_script.front());
    m_script.erase(m_script.begin());
    if (next.has_value() && request.onBody && next->status >= 200 && next->status < 300) {
        request.onBody(next->body);
        next->body.clear();
    }
    return next;
}
