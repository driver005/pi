#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

import std;
import pi.base.posix_byte_connection;
import pi.base.posix_unix_listener;

class EchoHandler : public IByteConnectionHandler {
public:
    explicit EchoHandler(std::shared_ptr<IByteConnection> connection, std::string closeWith = {})
        : m_connection(std::move(connection)), m_closeWith(std::move(closeWith)) {}

    void onData(std::string_view chunk) override {
        if (!m_closeWith.empty()) {
            m_connection->close(m_closeWith);
            return;
        }
        m_connection->send(chunk);
    }
    void onClose() override {
        ++m_closes;
    }
    void onError(const Error&) override {}

    std::atomic<int> m_closes{0};

private:
    std::shared_ptr<IByteConnection> m_connection;
    std::string m_closeWith;
};

class PosixUnixListenerTest : public ::testing::Test {
protected:
    PosixUnixListenerTest() {
        static std::atomic<int> counter{0};
        m_directory = "/tmp/pi-unix-" + std::to_string(::getpid()) + "-" + std::to_string(counter++);
        m_path = m_directory + "/server.sock";
    }

    ~PosixUnixListenerTest() override {
        if (m_listener) {
            m_listener->close();
        }
        std::error_code ec;
        std::filesystem::remove_all(m_directory, ec);
    }

    UnixListenerOptions options(std::int64_t graceful = 2000) const {
        UnixListenerOptions result;
        result.path = m_path;
        result.gracefulCloseTimeoutMs = graceful;
        result.connect = [](int fd, std::uint64_t limit, std::int64_t grace) {
            return std::shared_ptr<ISocketConnection>(std::make_shared<PosixByteConnection>(fd, limit, grace));
        };
        return result;
    }

    Result<void> startEcho(const UnixListenerOptions& listenerOptions, const std::string& closeWith = {}) {
        m_listener = std::make_unique<PosixUnixListener>(listenerOptions);
        return m_listener->start([this, closeWith](std::shared_ptr<IByteConnection> connection) {
            auto handler = std::make_shared<EchoHandler>(connection, closeWith);
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_handlers.push_back(handler);
            return std::shared_ptr<IByteConnectionHandler>(handler);
        });
    }

    int dial(const std::string& path) const {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un target{};
        target.sun_family = AF_UNIX;
        std::strncpy(target.sun_path, path.c_str(), sizeof(target.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    std::string readExactly(int fd, std::size_t size) const {
        std::string result;
        std::array<char, 4096> buffer{};
        while (result.size() < size) {
            const ssize_t n = ::recv(fd, buffer.data(), std::min(buffer.size(), size - result.size()), 0);
            if (n <= 0) {
                break;
            }
            result.append(buffer.data(), static_cast<std::size_t>(n));
        }
        return result;
    }

    int handlerCloses() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        int total = 0;
        for (const auto& handler : m_handlers) {
            total += handler->m_closes.load();
        }
        return total;
    }

    bool waitUntil(const std::function<bool()>& condition) const {
        for (int i = 0; i < 300; ++i) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return condition();
    }

    std::string m_directory;
    std::string m_path;
    std::mutex m_mutex;
    std::vector<std::shared_ptr<EchoHandler>> m_handlers;
    std::unique_ptr<PosixUnixListener> m_listener;
};

TEST_F(PosixUnixListenerTest, AcceptsClientsAndServesTheirBytes) {
    ASSERT_TRUE(startEcho(options()));
    const int fd = dial(m_path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::send(fd, "hello", 5, 0), 5);
    EXPECT_EQ(readExactly(fd, 5), "hello");
    ::close(fd);
    EXPECT_TRUE(waitUntil([&] { return handlerCloses() == 1; }));
}

TEST_F(PosixUnixListenerTest, LargePayloadsRoundTrip) {
    ASSERT_TRUE(startEcho(options()));
    const int fd = dial(m_path);
    ASSERT_GE(fd, 0);
    const std::string payload(1 << 20, 'q');
    std::thread writer([&] {
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const ssize_t n = ::send(fd, payload.data() + offset, payload.size() - offset, 0);
            ASSERT_GT(n, 0);
            offset += static_cast<std::size_t>(n);
        }
    });
    EXPECT_EQ(readExactly(fd, payload.size()).size(), payload.size());
    writer.join();
    ::close(fd);
}

TEST_F(PosixUnixListenerTest, ClosingWithAFinalChunkDeliversItThenHangsUp) {
    ASSERT_TRUE(startEcho(options(), "goodbye"));
    const int fd = dial(m_path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::send(fd, "x", 1, 0), 1);
    EXPECT_EQ(readExactly(fd, 7), "goodbye");
    char byte = 0;
    EXPECT_EQ(::recv(fd, &byte, 1, 0), 0);
    ::close(fd);
}

TEST_F(PosixUnixListenerTest, TheSocketIsPrivate) {
    ASSERT_TRUE(startEcho(options()));
    struct stat file {};
    ASSERT_EQ(::stat(m_path.c_str(), &file), 0);
    EXPECT_TRUE(S_ISSOCK(file.st_mode));
    EXPECT_EQ(file.st_mode & 0777, 0600u);
    struct stat directory {};
    ASSERT_EQ(::stat(m_directory.c_str(), &directory), 0);
    EXPECT_EQ(directory.st_mode & 0777, 0700u);
}

TEST_F(PosixUnixListenerTest, AStaleSocketFileIsReplaced) {
    std::filesystem::create_directories(m_directory);
    const int stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un target{};
    target.sun_family = AF_UNIX;
    std::strncpy(target.sun_path, m_path.c_str(), sizeof(target.sun_path) - 1);
    ASSERT_EQ(::bind(stale, reinterpret_cast<sockaddr*>(&target), sizeof(target)), 0);
    ::close(stale);
    ASSERT_TRUE(std::filesystem::exists(m_path));
    ASSERT_TRUE(startEcho(options()));
    const int fd = dial(m_path);
    EXPECT_GE(fd, 0);
    ::close(fd);
}

TEST_F(PosixUnixListenerTest, ALiveListenerIsNeverReplaced) {
    ASSERT_TRUE(startEcho(options()));
    PosixUnixListener second(options());
    auto result = second.start([](std::shared_ptr<IByteConnection>) { return std::shared_ptr<IByteConnectionHandler>(); });
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("already running"), std::string::npos);
    const int fd = dial(m_path);
    EXPECT_GE(fd, 0);
    ::close(fd);
}

TEST_F(PosixUnixListenerTest, NonSocketFilesAreNeverRemoved) {
    std::filesystem::create_directories(m_directory);
    std::ofstream(m_path) << "precious";
    auto result = startEcho(options());
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("non-socket"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(m_path));
}

TEST_F(PosixUnixListenerTest, TooLongPathsAreRefused) {
    UnixListenerOptions listenerOptions = options();
    listenerOptions.path = m_directory + "/" + std::string(200, 'x') + ".sock";
    auto result = startEcho(listenerOptions);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("too long"), std::string::npos);
}

TEST_F(PosixUnixListenerTest, ListenersNeedAConnectionFactory) {
    UnixListenerOptions listenerOptions = options();
    listenerOptions.connect = nullptr;
    EXPECT_EQ(startEcho(listenerOptions).error().code, "invalid_options");
}

TEST_F(PosixUnixListenerTest, CloseHangsUpClientsAndRemovesTheSocket) {
    ASSERT_TRUE(startEcho(options(100)));
    const int fd = dial(m_path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::send(fd, "a", 1, 0), 1);
    EXPECT_EQ(readExactly(fd, 1), "a");
    m_listener->close();
    char byte = 0;
    EXPECT_EQ(::recv(fd, &byte, 1, 0), 0);
    ::close(fd);
    EXPECT_FALSE(std::filesystem::exists(m_path));
    EXPECT_EQ(handlerCloses(), 1);
    m_listener->close();
}

TEST_F(PosixUnixListenerTest, CloseLeavesAReplacementSocketAlone) {
    ASSERT_TRUE(startEcho(options(100)));
    ASSERT_EQ(::unlink(m_path.c_str()), 0);
    std::ofstream(m_path) << "someone else";
    m_listener->close();
    EXPECT_TRUE(std::filesystem::exists(m_path));
}

TEST_F(PosixUnixListenerTest, ClosedListenersCannotRestart) {
    ASSERT_TRUE(startEcho(options(100)));
    m_listener->close();
    auto result = m_listener->start([](std::shared_ptr<IByteConnection>) { return std::shared_ptr<IByteConnectionHandler>(); });
    EXPECT_FALSE(result);
}
