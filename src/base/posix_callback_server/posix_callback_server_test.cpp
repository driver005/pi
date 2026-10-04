#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import pi.base.posix_callback_server;

class PosixCallbackServerTest : public testing::Test {
protected:
    /** One HTTP exchange against the server: the whole response text. */
    std::string get(int port, const std::string& target) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        EXPECT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
        const std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        EXPECT_EQ(::send(fd, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));
        std::string reply;
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                break;
            }
            reply.append(buffer.data(), static_cast<std::size_t>(received));
        }
        ::close(fd);
        return reply;
    }

    PosixCallbackServer m_server;
    std::shared_ptr<AbortSignal> m_signal = std::make_shared<AbortSignal>();
};

TEST_F(PosixCallbackServerTest, ReceivesTheRedirectOfTheAwaitedSignIn) {
    const auto port = m_server.listen("127.0.0.1", 0, false);
    ASSERT_TRUE(port.has_value()) << port.error().message;
    EXPECT_GT(*port, 0);
    std::string reply;
    std::thread browser([&] { reply = get(*port, "/callback?code=abc%2B1&state=s1&iss=https%3A%2F%2Fauth.example.com"); });
    const auto callback = m_server.waitForCallback({"/callback"}, "s1", std::chrono::seconds(5), m_signal);
    browser.join();
    ASSERT_TRUE(callback.has_value()) << callback.error().message;
    EXPECT_EQ((*callback)["code"], "abc+1");
    EXPECT_EQ((*callback)["iss"], "https://auth.example.com");
    EXPECT_NE(reply.find("200 OK"), std::string::npos);
    EXPECT_NE(reply.find("Signed in to the MCP server"), std::string::npos);
}

TEST_F(PosixCallbackServerTest, OtherPathsAndOtherStatesAreAnsweredAndIgnored) {
    const auto port = m_server.listen("127.0.0.1", 0, false);
    ASSERT_TRUE(port.has_value());
    std::vector<std::string> replies;
    std::thread browser([&] {
        replies.push_back(get(*port, "/favicon.ico"));
        replies.push_back(get(*port, "/callback?code=x&state=other"));
        replies.push_back(get(*port, "/callback?code=good&state=s1"));
    });
    const auto callback = m_server.waitForCallback({"/callback"}, "s1", std::chrono::seconds(5), m_signal);
    browser.join();
    ASSERT_TRUE(callback.has_value());
    EXPECT_EQ((*callback)["code"], "good");
    ASSERT_EQ(replies.size(), 3u);
    EXPECT_NE(replies[0].find("404"), std::string::npos);
    EXPECT_NE(replies[1].find("400"), std::string::npos);
    EXPECT_NE(replies[1].find("does not belong to this sign-in"), std::string::npos);
}

TEST_F(PosixCallbackServerTest, AuthorizationErrorsAreReturnedAndShownToTheUser) {
    const auto port = m_server.listen("127.0.0.1", 0, false);
    ASSERT_TRUE(port.has_value());
    std::string reply;
    std::thread browser([&] { reply = get(*port, "/callback?error=access_denied&error_description=User%20said%20%3Cno%3E&state=s1"); });
    const auto callback = m_server.waitForCallback({"/callback"}, "s1", std::chrono::seconds(5), m_signal);
    browser.join();
    ASSERT_TRUE(callback.has_value());
    EXPECT_EQ((*callback)["error"], "access_denied");
    EXPECT_NE(reply.find("User said &lt;no&gt;"), std::string::npos);
}

TEST_F(PosixCallbackServerTest, ATakenPortFallsBackUnlessItIsRequired) {
    PosixCallbackServer first;
    const auto taken = first.listen("127.0.0.1", 0, false);
    ASSERT_TRUE(taken.has_value());
    const auto fallback = m_server.listen("127.0.0.1", *taken, false);
    ASSERT_TRUE(fallback.has_value());
    EXPECT_NE(*fallback, *taken);
    PosixCallbackServer strict;
    EXPECT_FALSE(strict.listen("127.0.0.1", *taken, true).has_value());
}

TEST_F(PosixCallbackServerTest, WaitingEndsOnTimeoutAndAbort) {
    ASSERT_TRUE(m_server.listen("localhost", 0, false).has_value());
    const auto timedOut = m_server.waitForCallback({"/callback"}, "s", std::chrono::milliseconds(150), m_signal);
    ASSERT_FALSE(timedOut.has_value());
    EXPECT_EQ(timedOut.error().code, "timeout");
    std::thread aborter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        m_signal->abort();
    });
    const auto aborted = m_server.waitForCallback({"/callback"}, "s", std::chrono::seconds(10), m_signal);
    aborter.join();
    ASSERT_FALSE(aborted.has_value());
    EXPECT_EQ(aborted.error().code, "aborted");
}

TEST_F(PosixCallbackServerTest, WaitingBeforeListeningFails) {
    EXPECT_FALSE(m_server.waitForCallback({"/callback"}, "s", std::chrono::milliseconds(10), m_signal).has_value());
}

TEST_F(PosixCallbackServerTest, ListensOnTheIpv6LoopbackToo) {
    const auto port = m_server.listen("::1", 0, false);
    if (!port) {
        GTEST_SKIP() << "no IPv6 loopback: " << port.error().message;
    }
    EXPECT_GT(*port, 0);
}
