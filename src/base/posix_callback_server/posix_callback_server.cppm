module;

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>

export module pi.base.posix_callback_server;

import std;
export import pi.platform.i_callback_server;
import pi.support.url_parser;

/**
 * ICallbackServer over a blocking loopback TCP socket: requests are read and answered one at a time on the waiting thread,
 * with short timeouts so a stalled browser connection cannot block the sign-in.
 */
export class PosixCallbackServer : public ICallbackServer {
public:
    ~PosixCallbackServer() override {
        close();
    }

    Result<int> listen(const std::string& host, int port, bool required) override {
        close();
        const bool v6 = host == "::1";
        auto bound = bindTo(v6, port);
        if (!bound && !required && port != 0) {
            bound = bindTo(v6, 0);
        }
        if (!bound) {
            return std::unexpected(bound.error());
        }
        m_fd = *bound;
        return boundPort(v6);
    }

    Result<Json> waitForCallback(const std::vector<std::string>& paths, const std::string& state, std::chrono::milliseconds timeout, const std::shared_ptr<AbortSignal>& signal) override {
        if (m_fd < 0) {
            return std::unexpected(Error{"callback_server", "The callback server is not listening"});
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (signal && signal->aborted()) {
                return std::unexpected(Error{"aborted", "The sign-in was cancelled"});
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::unexpected(Error{"timeout", "Timed out waiting for the browser to complete the sign-in"});
            }
            pollfd waiting{m_fd, POLLIN, 0};
            if (::poll(&waiting, 1, 100) <= 0) {
                continue;
            }
            const int client = ::accept(m_fd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            auto outcome = serve(client, paths, state);
            ::close(client);
            if (outcome) {
                return *outcome;
            }
        }
    }

    void close() override {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

private:
    Result<int> bindTo(bool v6, int port) const {
        const int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::unexpected(Error{"callback_server", std::string("Unable to create a socket: ") + std::strerror(errno)});
        }
        const int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int status = -1;
        if (v6) {
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_addr = in6addr_loopback;
            address.sin6_port = htons(static_cast<std::uint16_t>(port));
            status = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        } else {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(static_cast<std::uint16_t>(port));
            status = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        }
        if (status != 0 || ::listen(fd, 8) != 0) {
            const std::string reason = std::strerror(errno);
            ::close(fd);
            return std::unexpected(Error{"callback_server", "Unable to listen on port " + std::to_string(port) + ": " + reason});
        }
        return fd;
    }

    Result<int> boundPort(bool v6) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            return std::unexpected(Error{"callback_server", std::string("Unable to read the port: ") + std::strerror(errno)});
        }
        return v6 ? ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port) : ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    }

    /** Answers one connection; the callback's parameters when it was the awaited one. */
    std::optional<Json> serve(int client, const std::vector<std::string>& paths, const std::string& state) {
        const std::string request = readRequest(client);
        const std::size_t lineEnd = request.find("\r\n");
        const std::string line = request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);
        const std::size_t firstSpace = line.find(' ');
        const std::size_t secondSpace = line.find(' ', firstSpace == std::string::npos ? 0 : firstSpace + 1);
        if (firstSpace == std::string::npos || secondSpace == std::string::npos || line.substr(0, firstSpace) != "GET") {
            respond(client, 400, errorPage("Bad request"));
            return std::nullopt;
        }
        const std::string target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        const std::size_t queryAt = target.find('?');
        const std::string path = target.substr(0, queryAt);
        if (std::ranges::find(paths, path) == paths.end()) {
            respond(client, 404, errorPage("Not found"));
            return std::nullopt;
        }
        Json parameters = Json::object();
        for (const auto& [name, value] : m_urls.parseQuery(queryAt == std::string::npos ? "" : target.substr(queryAt + 1))) {
            parameters[name] = value;
        }
        if (parameters.value("state", std::string()) != state) {
            respond(client, 400, errorPage("The sign-in response does not belong to this sign-in."));
            return std::nullopt;
        }
        if (parameters.contains("error")) {
            respond(client, 400, errorPage(parameters.value("error_description", parameters["error"].get<std::string>())));
            return parameters;
        }
        if (!parameters.contains("code")) {
            respond(client, 400, errorPage("The sign-in response has no authorization code."));
            return std::nullopt;
        }
        respond(client, 200, page("Signed in", "Signed in to the MCP server. You may now close this page."));
        return parameters;
    }

    std::string readRequest(int client) const {
        std::string request;
        std::array<char, 4096> buffer{};
        while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos) {
            pollfd waiting{client, POLLIN, 0};
            if (::poll(&waiting, 1, 2000) <= 0) {
                break;
            }
            const ssize_t received = ::recv(client, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                break;
            }
            request.append(buffer.data(), static_cast<std::size_t>(received));
        }
        return request;
    }

    void respond(int client, int status, const std::string& body) const {
        const std::string head = std::format("HTTP/1.1 {} {}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", status, status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request", body.size());
        const std::string reply = head + body;
        std::size_t offset = 0;
        while (offset < reply.size()) {
            const ssize_t written = ::send(client, reply.data() + offset, reply.size() - offset, MSG_NOSIGNAL);
            if (written <= 0) {
                return;
            }
            offset += static_cast<std::size_t>(written);
        }
    }

    std::string page(const std::string& title, const std::string& message) const {
        return "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + escape(title) + "</title></head><body style=\"font-family:sans-serif;margin:3em\"><h1>" +
               escape(title) + "</h1><p>" + escape(message) + "</p></body></html>";
    }

    std::string errorPage(const std::string& message) const {
        return page("Sign-in failed", message);
    }

    std::string escape(const std::string& text) const {
        std::string out;
        for (const char c : text) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
            }
        }
        return out;
    }

    int m_fd = -1;
    UrlParser m_urls;
};
