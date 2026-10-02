module;

#include <nlohmann/json.hpp>

export module pi.mcp.streamable_http_mcp_transport;

import std;
export import pi.mcp.i_mcp_transport;
export import pi.platform.i_http_client;
export import pi.platform.i_sleeper;
export import pi.support.abort_signal;
export import pi.support.header_merger;
export import pi.support.mcp_message_codec;
export import pi.support.sse_parser;
export import pi.types.mcp_http_exchange;
export import pi.types.mcp_http_options;
export import pi.types.mcp_http_worker;
export import pi.types.mcp_stream_cursor;
export import pi.types.mcp_stream_outcome;

/**
 * MCP over streamable HTTP: every client message is a POST; the reply comes back as JSON or as an
 * SSE stream, and an optional long-lived GET stream carries server-initiated messages. Dropped
 * streams that carried event ids are resumed with Last-Event-ID. Error codes returned by send():
 * "auth_required" (401), "session_expired" (404 with a session), "http:<status>", "protocol",
 * "closed", plus the HTTP client's own codes. Port of packages/mcp/src/transports/streamable-http.ts
 * (OAuth step-up through an auth provider is not ported; use options.bearerToken).
 */
export class StreamableHttpMcpTransport : public IMcpTransport {
public:
    StreamableHttpMcpTransport(IHttpClient& http, ISleeper& sleeper, McpHttpOptions options);
    ~StreamableHttpMcpTransport() override;

    Result<void> start() override;
    Result<void> send(const Json& message) override;
    void close() override;
    void setMessageListener(MessageListener listener) override;
    void setErrorListener(ErrorListener listener) override;
    void setCloseListener(CloseListener listener) override;
    void setProtocolVersion(const std::string& version) override;

    /** The session id the server assigned, or empty. */
    std::string sessionId() const;

private:
    Result<void> sendNotification(const Json& message);
    Result<void> sendRequest(const Json& message);
    Result<void> awaitExchange(McpHttpExchange& exchange);
    void publishExchange(McpHttpExchange& exchange, const std::optional<Error>& error);
    bool exchangeFailed(McpHttpExchange& exchange);
    void acceptResponse(McpHttpExchange& exchange, int status, const std::string& type);

    void runRequest(const Json& message, McpHttpExchange& exchange);
    void resumeResponse(McpStreamCursor& cursor, McpStreamOutcome outcome);
    void runGetStream();
    void startGetStream();
    void deleteSession();
    void joinWorkers(std::vector<McpHttpWorker>& workers);
    void pruneWorkers();

    McpStreamOutcome streamOnce(HttpRequest request, McpHttpExchange* exchange,
                                McpStreamCursor& cursor);
    McpStreamOutcome finishExchange(const HttpResponse& response, bool sse, bool json,
                                    const std::string& jsonBody, bool tooLarge, SseParser& parser,
                                    McpStreamCursor& cursor, const std::string& method);
    McpStreamOutcome failedOutcome(const Error& error, bool tooLarge) const;
    void deliverEvents(const std::vector<SseEvent>& events, McpStreamCursor& cursor);
    void deliverMessage(const Json& message, McpStreamCursor& cursor);
    void deliverJsonBody(const std::string& body, McpStreamCursor& cursor);
    void trackCursor(const SseParser& parser, McpStreamCursor& cursor) const;
    void failRequest(const Json& id, const std::string& reason);

    HttpRequest buildRequest(const std::string& method, const std::string& accept,
                             const std::string& lastEventId, const std::string& body) const;
    void captureSession(const HttpHeaders& headers);
    std::string contentType(const HttpHeaders& headers) const;
    Error statusError(int status, const std::string& body) const;
    bool transientStatus(int status) const;
    std::chrono::milliseconds reconnectDelay(int attempt, std::optional<int> serverDelayMs) const;
    bool isClosed() const;

    void emitMessage(const Json& message);
    void emitError(const Error& error);
    void emitClose();

    IHttpClient& m_http;
    ISleeper& m_sleeper;
    McpHttpOptions m_options;
    McpMessageCodec m_codec;
    HeaderMerger m_headers;
    std::shared_ptr<AbortSignal> m_abort = std::make_shared<AbortSignal>();
    mutable std::mutex m_mutex;
    MessageListener m_message;
    ErrorListener m_error;
    CloseListener m_close;
    std::string m_sessionId;
    std::string m_protocolVersion;
    bool m_started = false;
    bool m_closed = false;
    bool m_closeEmitted = false;
    bool m_getStarted = false;
    std::vector<McpHttpWorker> m_workers;
    McpHttpWorker m_getStream;
};

StreamableHttpMcpTransport::StreamableHttpMcpTransport(IHttpClient& http, ISleeper& sleeper,
                                                       McpHttpOptions options)
    : m_http(http), m_sleeper(sleeper), m_options(std::move(options)) {}

StreamableHttpMcpTransport::~StreamableHttpMcpTransport() {
    close();
}

void StreamableHttpMcpTransport::setMessageListener(MessageListener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_message = std::move(listener);
}

void StreamableHttpMcpTransport::setErrorListener(ErrorListener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_error = std::move(listener);
}

void StreamableHttpMcpTransport::setCloseListener(CloseListener listener) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_close = std::move(listener);
}

void StreamableHttpMcpTransport::setProtocolVersion(const std::string& version) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_protocolVersion = version;
}

std::string StreamableHttpMcpTransport::sessionId() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessionId;
}

bool StreamableHttpMcpTransport::isClosed() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

void StreamableHttpMcpTransport::emitMessage(const Json& message) {
    MessageListener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        listener = m_message;
    }
    if (listener) {
        listener(message);
    }
}

void StreamableHttpMcpTransport::emitError(const Error& error) {
    ErrorListener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        listener = m_error;
    }
    if (listener) {
        listener(error);
    }
}

void StreamableHttpMcpTransport::emitClose() {
    CloseListener listener;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closeEmitted) {
            return;
        }
        m_closeEmitted = true;
        listener = m_close;
    }
    if (listener) {
        listener();
    }
}

Result<void> StreamableHttpMcpTransport::start() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started) {
        return std::unexpected(Error{"closed", "MCP Streamable HTTP transport already started"});
    }
    if (m_closed) {
        return std::unexpected(Error{"closed", "MCP connection closed"});
    }
    m_started = true;
    return {};
}

std::string StreamableHttpMcpTransport::contentType(const HttpHeaders& headers) const {
    std::string value = m_headers.find(headers, "content-type").value_or("");
    value = value.substr(0, value.find(';'));
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    value = value.substr(first, value.find_last_not_of(" \t") - first + 1);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void StreamableHttpMcpTransport::captureSession(const HttpHeaders& headers) {
    const auto id = m_headers.find(headers, "mcp-session-id");
    if (id && !id->empty()) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_sessionId = *id;
    }
}

bool StreamableHttpMcpTransport::transientStatus(int status) const {
    return status == 408 || status == 429 || status >= 500;
}

Error StreamableHttpMcpTransport::statusError(int status, const std::string& body) const {
    std::string snippet = body.substr(0, 8 * 1024);
    const std::size_t first = snippet.find_first_not_of(" \t\r\n");
    snippet = first == std::string::npos ? "" : snippet.substr(first);
    while (!snippet.empty() && std::isspace(static_cast<unsigned char>(snippet.back())) != 0) {
        snippet.pop_back();
    }
    if (snippet.size() > 500) {
        snippet = snippet.substr(0, 497) + "...";
    }
    if (status == 401) {
        return Error{"auth_required", "MCP server requires authentication"};
    }
    if (status == 404 && !sessionId().empty()) {
        return Error{"session_expired", "MCP session expired"};
    }
    return Error{"http:" + std::to_string(status),
                 "MCP HTTP request failed with status " + std::to_string(status) +
                     (snippet.empty() ? "" : ": " + snippet)};
}

std::chrono::milliseconds StreamableHttpMcpTransport::reconnectDelay(
    int attempt, std::optional<int> serverDelayMs) const {
    if (serverDelayMs) {
        return std::chrono::milliseconds(*serverDelayMs);
    }
    const std::int64_t scaled = m_options.reconnectInitialDelayMs << std::min(attempt, 20);
    return std::chrono::milliseconds(std::min(scaled, m_options.reconnectMaxDelayMs));
}

HttpRequest StreamableHttpMcpTransport::buildRequest(const std::string& method,
                                                     const std::string& accept,
                                                     const std::string& lastEventId,
                                                     const std::string& body) const {
    HttpRequest request;
    request.method = method;
    request.url = m_options.url;
    request.headers = m_options.headers;
    m_headers.set(request.headers, "accept", accept);
    if (!body.empty()) {
        m_headers.set(request.headers, "content-type", "application/json");
        request.body = body;
    }
    std::string protocol;
    std::string session;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        protocol = m_protocolVersion;
        session = m_sessionId;
    }
    if (!session.empty()) {
        m_headers.set(request.headers, "Mcp-Session-Id", session);
    }
    if (!protocol.empty()) {
        m_headers.set(request.headers, "MCP-Protocol-Version", protocol);
    }
    if (!lastEventId.empty()) {
        m_headers.set(request.headers, "last-event-id", lastEventId);
    }
    const std::string token = m_options.bearerToken ? m_options.bearerToken() : "";
    if (!token.empty()) {
        m_headers.set(request.headers, "authorization", "Bearer " + token);
    }
    return request;
}

Result<void> StreamableHttpMcpTransport::send(const Json& message) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started || m_closed) {
            return std::unexpected(Error{"closed", "MCP connection closed"});
        }
    }
    if (m_codec.isRequest(message)) {
        return sendRequest(message);
    }
    return sendNotification(message);
}

Result<void> StreamableHttpMcpTransport::sendNotification(const Json& message) {
    HttpRequest request = buildRequest("POST", "application/json, text/event-stream", "",
                                       message.dump(-1, ' ', false, Json::error_handler_t::replace));
    request.signal = m_abort;
    const auto response = m_http.send(request);
    if (!response) {
        return std::unexpected(response.error());
    }
    captureSession(response->headers);
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(statusError(response->status, response->body));
    }
    if (message.value("method", "") == "notifications/initialized") {
        startGetStream();
    }
    return {};
}

void StreamableHttpMcpTransport::joinWorkers(std::vector<McpHttpWorker>& workers) {
    for (McpHttpWorker& worker : workers) {
        if (!worker.thread.joinable()) {
            continue;
        }
        if (worker.thread.get_id() == std::this_thread::get_id()) {
            worker.thread.detach();
        } else {
            worker.thread.join();
        }
    }
}

void StreamableHttpMcpTransport::pruneWorkers() {
    std::vector<McpHttpWorker> done;
    std::vector<McpHttpWorker> running;
    for (McpHttpWorker& worker : m_workers) {
        (worker.finished->load() ? done : running).push_back(std::move(worker));
    }
    m_workers = std::move(running);
    joinWorkers(done);
}

Result<void> StreamableHttpMcpTransport::sendRequest(const Json& message) {
    auto exchange = std::make_shared<McpHttpExchange>();
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return std::unexpected(Error{"closed", "MCP connection closed"});
        }
        pruneWorkers();
        McpHttpWorker worker;
        worker.finished = std::make_shared<std::atomic<bool>>(false);
        worker.thread = std::thread([this, message, exchange, finished = worker.finished]() {
            runRequest(message, *exchange);
            finished->store(true);
        });
        m_workers.push_back(std::move(worker));
    }
    return awaitExchange(*exchange);
}

Result<void> StreamableHttpMcpTransport::awaitExchange(McpHttpExchange& exchange) {
    std::unique_lock<std::mutex> lock(exchange.mutex);
    exchange.ready.wait(lock, [&exchange]() { return exchange.published; });
    if (exchange.error) {
        return std::unexpected(*exchange.error);
    }
    return {};
}

void StreamableHttpMcpTransport::publishExchange(McpHttpExchange& exchange,
                                                 const std::optional<Error>& error) {
    {
        const std::lock_guard<std::mutex> lock(exchange.mutex);
        if (exchange.published) {
            return;
        }
        exchange.published = true;
        exchange.error = error;
    }
    exchange.ready.notify_all();
}

bool StreamableHttpMcpTransport::exchangeFailed(McpHttpExchange& exchange) {
    const std::lock_guard<std::mutex> lock(exchange.mutex);
    return exchange.error.has_value();
}

void StreamableHttpMcpTransport::acceptResponse(McpHttpExchange& exchange, int status,
                                                const std::string& type) {
    if (status == 202 || status == 204) {
        publishExchange(exchange, Error{"http:" + std::to_string(status),
                                        "MCP server accepted the request without a response"});
    } else if (type != "application/json" && type != "text/event-stream") {
        publishExchange(exchange,
                        Error{"protocol", "Unsupported MCP response content type: " +
                                              (type.empty() ? std::string("missing") : type)});
    } else {
        publishExchange(exchange, std::nullopt);
    }
}

void StreamableHttpMcpTransport::trackCursor(const SseParser& parser,
                                             McpStreamCursor& cursor) const {
    if (!parser.lastEventId().empty()) {
        cursor.lastEventId = parser.lastEventId();
    }
    if (parser.lastRetryMs()) {
        cursor.retryMs = parser.lastRetryMs();
    }
}

void StreamableHttpMcpTransport::deliverMessage(const Json& message, McpStreamCursor& cursor) {
    if (m_codec.isResponse(message) && !cursor.awaitedId.is_null() &&
        message.value("id", Json()) == cursor.awaitedId) {
        cursor.answered = true;
    }
    emitMessage(message);
}

void StreamableHttpMcpTransport::deliverEvents(const std::vector<SseEvent>& events,
                                               McpStreamCursor& cursor) {
    for (const SseEvent& event : events) {
        cursor.received = true;
        if (event.data.find_first_not_of(" \t\r\n") == std::string::npos || event.event != "message") {
            continue;
        }
        const Json message = Json::parse(event.data, nullptr, false);
        if (!(m_codec.isRequest(message) || m_codec.isNotification(message) ||
              m_codec.isResponse(message))) {
            emitError(Error{"protocol", "Invalid JSON-RPC message: " + event.data.substr(0, 200)});
            continue;
        }
        deliverMessage(message, cursor);
    }
}

void StreamableHttpMcpTransport::deliverJsonBody(const std::string& body, McpStreamCursor& cursor) {
    const Json parsed = Json::parse(body, nullptr, false);
    const std::vector<Json> items = parsed.is_array() ? parsed.get<std::vector<Json>>()
                                                      : std::vector<Json>{parsed};
    for (const Json& item : items) {
        if (!(m_codec.isRequest(item) || m_codec.isNotification(item) || m_codec.isResponse(item))) {
            emitError(Error{"protocol", "Invalid JSON-RPC message: " + body.substr(0, 200)});
            continue;
        }
        cursor.received = true;
        deliverMessage(item, cursor);
    }
}

McpStreamOutcome StreamableHttpMcpTransport::failedOutcome(const Error& error,
                                                           bool tooLarge) const {
    McpStreamOutcome outcome;
    if (tooLarge) {
        outcome.error = Error{"protocol", "MCP message exceeds " +
                                              std::to_string(m_options.maxMessageBytes) + " bytes"};
        return outcome;
    }
    outcome.error = error;
    outcome.retryable = error.code != "aborted" && error.code != "closed";
    return outcome;
}

McpStreamOutcome StreamableHttpMcpTransport::finishExchange(
    const HttpResponse& response, bool sse, bool json, const std::string& jsonBody, bool tooLarge,
    SseParser& parser, McpStreamCursor& cursor, const std::string& method) {
    McpStreamOutcome outcome;
    if (tooLarge) {
        return failedOutcome(Error{}, true);
    }
    if (response.status == 405 && method == "GET") {
        outcome.unsupported = true;
        return outcome;
    }
    if (response.status < 200 || response.status >= 300) {
        outcome.error = statusError(response.status, response.body);
        outcome.retryable = transientStatus(response.status);
        return outcome;
    }
    if (sse) {
        deliverEvents(parser.finish(), cursor);
        trackCursor(parser, cursor);
    } else if (json) {
        deliverJsonBody(jsonBody, cursor);
    } else {
        const std::string type = contentType(response.headers);
        outcome.error = Error{"protocol", "Unsupported MCP response content type: " +
                                              (type.empty() ? std::string("missing") : type)};
    }
    return outcome;
}

McpStreamOutcome StreamableHttpMcpTransport::streamOnce(HttpRequest request,
                                                        McpHttpExchange* exchange,
                                                        McpStreamCursor& cursor) {
    const auto signal = std::make_shared<AbortSignal>();
    const std::uint64_t link = m_abort->onAbort([signal]() { signal->abort(); });
    request.signal = signal;
    SseParser parser;
    std::string jsonBody;
    std::size_t pending = 0;
    bool sse = false;
    bool json = false;
    bool tooLarge = false;
    request.onResponse = [&](int status, const HttpHeaders& headers) {
        captureSession(headers);
        const std::string type = contentType(headers);
        sse = type == "text/event-stream";
        json = type == "application/json";
        if (exchange != nullptr && status >= 200 && status < 300) {
            acceptResponse(*exchange, status, type);
        }
    };
    request.onBody = [&](std::string_view chunk) {
        pending += chunk.size();
        if (sse) {
            const auto events = parser.feed(chunk);
            pending = events.empty() ? pending : 0;
            trackCursor(parser, cursor);
            deliverEvents(events, cursor);
        } else if (json) {
            jsonBody.append(chunk);
            pending = jsonBody.size();
        }
        if (pending > m_options.maxMessageBytes) {
            tooLarge = true;
            signal->abort();
        }
    };
    const std::string method = request.method;
    const auto result = m_http.send(request);
    m_abort->removeListener(link);
    if (!result) {
        return failedOutcome(result.error(), tooLarge);
    }
    return finishExchange(*result, sse, json, jsonBody, tooLarge, parser, cursor, method);
}

void StreamableHttpMcpTransport::failRequest(const Json& id, const std::string& reason) {
    emitMessage(m_codec.error(id, McpMessageCodec::InternalError,
                              "MCP response stream failed: " + reason));
}

void StreamableHttpMcpTransport::runRequest(const Json& message, McpHttpExchange& exchange) {
    McpStreamCursor cursor;
    cursor.awaitedId = message.value("id", Json());
    const McpStreamOutcome outcome = streamOnce(
        buildRequest("POST", "application/json, text/event-stream", "",
                     message.dump(-1, ' ', false, Json::error_handler_t::replace)),
        &exchange, cursor);
    publishExchange(exchange, outcome.error);
    if (exchangeFailed(exchange)) {
        // send() already reported the failure to the caller.
        return;
    }
    resumeResponse(cursor, outcome);
}

void StreamableHttpMcpTransport::resumeResponse(McpStreamCursor& cursor, McpStreamOutcome outcome) {
    int attempt = 0;
    while (!cursor.answered && !isClosed()) {
        if ((outcome.error && !outcome.retryable) || cursor.lastEventId.empty() ||
            attempt >= m_options.reconnectMaxRetries) {
            break;
        }
        if (cursor.received) {
            attempt = 0;
        }
        cursor.received = false;
        if (!m_sleeper.sleep(reconnectDelay(attempt++, cursor.retryMs), m_abort)) {
            return;
        }
        outcome = streamOnce(buildRequest("GET", "text/event-stream", cursor.lastEventId, ""),
                             nullptr, cursor);
        if (outcome.unsupported) {
            break;
        }
    }
    if (cursor.answered || isClosed()) {
        return;
    }
    failRequest(cursor.awaitedId,
                outcome.error ? outcome.error->message : "stream ended without a response");
}

void StreamableHttpMcpTransport::startGetStream() {
    if (!m_options.openGetStream) {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_getStarted || m_closed) {
        return;
    }
    m_getStarted = true;
    m_getStream.finished = std::make_shared<std::atomic<bool>>(false);
    m_getStream.thread = std::thread([this, finished = m_getStream.finished]() {
        runGetStream();
        finished->store(true);
    });
}

void StreamableHttpMcpTransport::runGetStream() {
    McpStreamCursor cursor;
    int attempt = 0;
    while (!isClosed()) {
        const auto openedAt = std::chrono::steady_clock::now();
        const McpStreamOutcome outcome =
            streamOnce(buildRequest("GET", "text/event-stream", cursor.lastEventId, ""), nullptr, cursor);
        if (outcome.unsupported || isClosed()) {
            return;
        }
        if (outcome.error && !outcome.retryable) {
            emitError(*outcome.error);
            return;
        }
        const auto uptime = std::chrono::steady_clock::now() - openedAt;
        if (cursor.received || uptime > std::chrono::milliseconds(m_options.reconnectMaxDelayMs)) {
            attempt = 0;
        }
        cursor.received = false;
        if (attempt >= m_options.reconnectMaxRetries) {
            emitError(Error{"transport", "MCP server-to-client stream dropped and could not be reopened"});
            return;
        }
        if (!m_sleeper.sleep(reconnectDelay(attempt++, cursor.retryMs), m_abort)) {
            return;
        }
    }
}

void StreamableHttpMcpTransport::deleteSession() {
    HttpRequest request = buildRequest("DELETE", "application/json", "", "");
    request.timeout = std::chrono::milliseconds(m_options.closeTimeoutMs);
    m_http.send(request);
}

void StreamableHttpMcpTransport::close() {
    std::vector<McpHttpWorker> workers;
    bool deleteNeeded = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return;
        }
        m_closed = true;
        deleteNeeded = m_started && !m_sessionId.empty();
        workers = std::move(m_workers);
        m_workers.clear();
        workers.push_back(std::move(m_getStream));
    }
    m_abort->abort();
    joinWorkers(workers);
    if (deleteNeeded) {
        deleteSession();
    }
    emitClose();
}
