#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

import std;
import pi.base.posix_byte_connection;

class CollectingHandler : public IByteConnectionHandler {
public:
    void onData(std::string_view chunk) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_data.append(chunk);
        m_changed.notify_all();
    }
    void onClose() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_changed.notify_all();
    }
    void onError(const Error&) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_errors++;
    }
    bool waitForData(std::size_t size) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(3), [&] { return m_data.size() >= size; });
    }
    bool waitClosed() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(3), [&] { return m_closed; });
    }
    std::string data() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_data;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::string m_data;
    bool m_closed = false;
    int m_errors = 0;
};

class PosixByteConnectionTest : public ::testing::Test {
protected:
    void open(std::uint64_t limit = 1 << 20, std::int64_t graceful = 2000) {
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        m_peer = fds[1];
        m_handler = std::make_shared<CollectingHandler>();
        m_connection = std::make_shared<PosixByteConnection>(fds[0], limit, graceful);
        m_reader = std::thread([this] { m_connection->run(m_handler); });
    }

    ~PosixByteConnectionTest() override {
        if (m_connection) {
            m_connection->close();
        }
        if (m_peer >= 0) {
            ::close(m_peer);
        }
        if (m_reader.joinable()) {
            m_reader.join();
        }
    }

    std::string readAll() {
        std::string result;
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t n = ::recv(m_peer, buffer.data(), buffer.size(), 0);
            if (n <= 0) {
                return result;
            }
            result.append(buffer.data(), static_cast<std::size_t>(n));
        }
    }

    int m_peer = -1;
    std::shared_ptr<CollectingHandler> m_handler;
    std::shared_ptr<PosixByteConnection> m_connection;
    std::thread m_reader;
};

TEST_F(PosixByteConnectionTest, ReceivedBytesReachTheHandlerInOrder) {
    open();
    ASSERT_EQ(::send(m_peer, "abc", 3, 0), 3);
    ASSERT_EQ(::send(m_peer, "def", 3, 0), 3);
    ASSERT_TRUE(m_handler->waitForData(6));
    EXPECT_EQ(m_handler->data(), "abcdef");
}

TEST_F(PosixByteConnectionTest, SentBytesReachThePeer) {
    open();
    ASSERT_TRUE(m_connection->send("hello "));
    ASSERT_TRUE(m_connection->send("world"));
    m_connection->close();
    EXPECT_EQ(readAll(), "hello world");
}

TEST_F(PosixByteConnectionTest, CloseFlushesTheQueueAndTheFinalChunkThenHangsUp) {
    open();
    ASSERT_TRUE(m_connection->send("one "));
    m_connection->close("last");
    EXPECT_TRUE(m_connection->closed());
    EXPECT_EQ(m_connection->send("late").error().code, "connection_closed");
    std::string received;
    std::array<char, 64> buffer{};
    while (received.size() < 8) {
        const ssize_t n = ::recv(m_peer, buffer.data(), buffer.size(), 0);
        ASSERT_GT(n, 0);
        received.append(buffer.data(), static_cast<std::size_t>(n));
    }
    EXPECT_EQ(received, "one last");
    EXPECT_EQ(::recv(m_peer, buffer.data(), buffer.size(), 0), 0);
    ::close(m_peer);
    m_peer = -1;
    EXPECT_TRUE(m_handler->waitClosed());
}

TEST_F(PosixByteConnectionTest, APeerThatNeverHangsUpIsCutAfterTheGracePeriod) {
    open(1 << 20, 50);
    m_connection->close("bye");
    EXPECT_TRUE(m_handler->waitClosed());
}

TEST_F(PosixByteConnectionTest, PeerHangupEndsTheReadLoop) {
    open();
    ::close(m_peer);
    m_peer = -1;
    EXPECT_TRUE(m_handler->waitClosed());
    EXPECT_TRUE(m_connection->closed());
    EXPECT_FALSE(m_connection->send("x"));
}

TEST_F(PosixByteConnectionTest, SlowPeersAreRefusedPastThePendingLimit) {
    open(1024);
    EXPECT_EQ(m_connection->send(std::string(2000, 'x')).error().code, "connection_slow");
    EXPECT_TRUE(m_connection->send(std::string(500, 'y')));
}

TEST_F(PosixByteConnectionTest, LargeTransfersSurvivePartialWrites) {
    open(16 << 20);
    const std::string payload(4 << 20, 'z');
    ASSERT_TRUE(m_connection->send(payload));
    m_connection->close();
    EXPECT_EQ(readAll().size(), payload.size());
}
