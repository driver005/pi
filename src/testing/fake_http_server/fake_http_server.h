#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/testing/fake_http_reply/fake_http_reply.h"
#include "src/testing/fake_http_request/fake_http_request.h"

/**
 * Loopback HTTP/1.1 server for tests. Each connection serves one request and closes, so
 * streamed (SSE) bodies need no chunked framing. Not thread-safe to reconfigure while running.
 */
class FakeHttpServer {
public:
    using Handler = std::function<FakeHttpReply(const FakeHttpRequest&)>;

    explicit FakeHttpServer(Handler handler);
    ~FakeHttpServer();

    FakeHttpServer(const FakeHttpServer&) = delete;
    FakeHttpServer& operator=(const FakeHttpServer&) = delete;

    int port() const;
    std::string url(const std::string& path) const;
    std::vector<FakeHttpRequest> requests() const;

private:
    void acceptLoop();
    void serve(int client);
    bool readRequest(int client, FakeHttpRequest& out) const;
    void writeReply(int client, const FakeHttpReply& reply) const;
    bool sendAll(int client, const std::string& data) const;
    void sleepMs(int ms) const;

    Handler m_handler;
    int m_listenFd = -1;
    int m_port = 0;
    std::atomic<bool> m_stopping{false};
    std::thread m_acceptThread;
    std::vector<std::thread> m_workers;
    mutable std::mutex m_mutex;
    std::vector<FakeHttpRequest> m_requests;
};
