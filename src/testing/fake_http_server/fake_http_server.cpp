#include "src/testing/fake_http_server/fake_http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>

FakeHttpServer::FakeHttpServer(Handler handler) : m_handler(std::move(handler)) {
    m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    const int reuse = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(m_listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    listen(m_listenFd, 16);
    socklen_t length = sizeof(address);
    getsockname(m_listenFd, reinterpret_cast<sockaddr*>(&address), &length);
    m_port = ntohs(address.sin_port);
    m_acceptThread = std::thread([this] { acceptLoop(); });
}

FakeHttpServer::~FakeHttpServer() {
    m_stopping = true;
    shutdown(m_listenFd, SHUT_RDWR);
    m_acceptThread.join();
    close(m_listenFd);
    for (std::thread& worker : m_workers) {
        worker.join();
    }
}

int FakeHttpServer::port() const {
    return m_port;
}

std::string FakeHttpServer::url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(m_port) + path;
}

std::vector<FakeHttpRequest> FakeHttpServer::requests() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
}

void FakeHttpServer::acceptLoop() {
    while (!m_stopping) {
        const int client = accept(m_listenFd, nullptr, nullptr);
        if (client < 0) {
            return;
        }
        m_workers.emplace_back([this, client] { serve(client); });
    }
}

void FakeHttpServer::sleepMs(int ms) const {
    for (int waited = 0; waited < ms && !m_stopping; waited += 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10, ms - waited)));
    }
}

bool FakeHttpServer::readRequest(int client, FakeHttpRequest& out) const {
    std::string data;
    char buffer[4096];
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return false;
        }
        data.append(buffer, static_cast<std::size_t>(count));
        headerEnd = data.find("\r\n\r\n");
    }
    std::size_t lineStart = 0;
    std::size_t lineEnd = data.find("\r\n");
    const std::string requestLine = data.substr(0, lineEnd);
    const std::size_t firstSpace = requestLine.find(' ');
    const std::size_t secondSpace = requestLine.find(' ', firstSpace + 1);
    out.method = requestLine.substr(0, firstSpace);
    out.path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    std::size_t contentLength = 0;
    lineStart = lineEnd + 2;
    while (lineStart < headerEnd) {
        lineEnd = data.find("\r\n", lineStart);
        const std::string line = data.substr(lineStart, lineEnd - lineStart);
        const std::size_t colon = line.find(':');
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(' '));
        out.headers.emplace_back(line.substr(0, colon), value);
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name == "content-length") {
            contentLength = static_cast<std::size_t>(std::stoul(value));
        }
        lineStart = lineEnd + 2;
    }
    out.body = data.substr(headerEnd + 4);
    while (out.body.size() < contentLength) {
        const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return false;
        }
        out.body.append(buffer, static_cast<std::size_t>(count));
    }
    return true;
}

bool FakeHttpServer::sendAll(int client, const std::string& data) const {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = send(client, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void FakeHttpServer::writeReply(int client, const FakeHttpReply& reply) const {
    sleepMs(reply.initialDelayMs);
    std::string head = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nConnection: close\r\n";
    for (const auto& [name, value] : reply.headers) {
        head += name + ": " + value + "\r\n";
    }
    head += "\r\n";
    if (!sendAll(client, head)) {
        return;
    }
    for (const std::string& chunk : reply.chunks) {
        if (!sendAll(client, chunk)) {
            return;
        }
        sleepMs(reply.chunkDelayMs);
    }
    sleepMs(reply.tailDelayMs);
}

void FakeHttpServer::serve(int client) {
    FakeHttpRequest request;
    if (readRequest(client, request)) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_requests.push_back(request);
        }
        writeReply(client, m_handler(request));
    }
    close(client);
}
