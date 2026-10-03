#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

import std;
import pi.base.posix_byte_connection;
import pi.base.posix_unix_connector;

class PosixUnixConnectorTest : public testing::Test {
protected:
    PosixUnixConnectorTest() {
        m_path = "/tmp/pi-connector-" + std::to_string(::getpid()) + ".sock";
        ::unlink(m_path.c_str());
        m_listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, m_path.c_str(), sizeof(address.sun_path) - 1);
        EXPECT_EQ(::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        EXPECT_EQ(::listen(m_listener, 4), 0);
    }

    ~PosixUnixConnectorTest() override {
        ::close(m_listener);
        ::unlink(m_path.c_str());
    }

    PosixUnixConnector::Wrap wrap() {
        return [](int fd, std::uint64_t limit, std::int64_t grace) {
            return std::shared_ptr<ISocketConnection>(std::make_shared<PosixByteConnection>(fd, limit, grace));
        };
    }

    std::string m_path;
    int m_listener = -1;
};

TEST_F(PosixUnixConnectorTest, ExchangesBytesAndReportsTheCloseOnce) {
    std::mutex mutex;
    std::condition_variable changed;
    std::string received;
    int closes = 0;
    ClientTransportHandlers handlers;
    handlers.onData = [&](std::string_view chunk) {
        const std::lock_guard<std::mutex> lock(mutex);
        received.append(chunk);
        changed.notify_all();
    };
    handlers.onClose = [&] {
        const std::lock_guard<std::mutex> lock(mutex);
        ++closes;
        changed.notify_all();
    };
    PosixUnixConnector connector(m_path, wrap());
    auto connection = connector.connect(handlers);
    ASSERT_TRUE(connection.has_value()) << connection.error().message;
    const int peer = ::accept(m_listener, nullptr, nullptr);
    ASSERT_GE(peer, 0);
    ASSERT_EQ(::send(peer, "hello", 5, 0), 5);
    ASSERT_TRUE((*connection)->send("yo").has_value());
    std::array<char, 8> buffer{};
    ASSERT_EQ(::recv(peer, buffer.data(), buffer.size(), 0), 2);
    EXPECT_EQ(std::string(buffer.data(), 2), "yo");
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return received == "hello"; }));
    }
    ::close(peer);
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return closes == 1; }));
    EXPECT_TRUE((*connection)->closed());
}

TEST_F(PosixUnixConnectorTest, ClosingEndsTheReaderAndTheDestructorJoinsIt) {
    std::atomic<int> closes{0};
    ClientTransportHandlers handlers;
    handlers.onClose = [&] { ++closes; };
    {
        PosixUnixConnector connector(m_path, wrap(), 1024 * 1024, 200);
        auto connection = connector.connect(handlers);
        ASSERT_TRUE(connection.has_value());
        const int peer = ::accept(m_listener, nullptr, nullptr);
        ASSERT_GE(peer, 0);
        (*connection)->close();
        // The peer never hangs up; the graceful-close timeout cuts the connection.
        for (int i = 0; i < 500 && closes.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(closes.load(), 1);
        ::close(peer);
    }
}

TEST_F(PosixUnixConnectorTest, ConnectingToAMissingSocketFails) {
    PosixUnixConnector connector(m_path + ".missing", wrap());
    const auto connection = connector.connect(ClientTransportHandlers{});
    ASSERT_FALSE(connection.has_value());
    EXPECT_EQ(connection.error().code, "unix_connect");
}

TEST_F(PosixUnixConnectorTest, AnOverlongPathFails) {
    PosixUnixConnector connector(std::string(200, 'x'), wrap());
    EXPECT_FALSE(connector.connect(ClientTransportHandlers{}).has_value());
}
