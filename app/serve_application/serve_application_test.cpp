#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdlib>

import std;
import pi.ai.faux_provider;
import pi.coding_runtime_factory;
import pi.coding_services;
import pi.durable_serve;
import pi.base.posix_byte_connection;
import pi.base.posix_unix_connector;
import pi.serve_application;
import pi.support.protocol_client;
import pi.support.protocol_codec;
import pi.support.protocol_message_decoder;

/** Every test runs for both session backends: durable (SQLite harness) sessions and session trees. */
class ServeApplicationTest : public testing::TestWithParam<bool> {
protected:
    static constexpr const char* kServerId = "00000000-0000-4000-8000-000000000001";

    ServeApplicationTest() {
        const std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/serve_" + name;
        m_serverDir = "/tmp/pi-serve-" + std::to_string(::getpid()) + "-" + std::to_string(std::hash<std::string>{}(name) % 100000);
        std::filesystem::remove_all(m_dir);
        std::filesystem::remove_all(m_serverDir);
        std::filesystem::create_directories(m_dir + "/project");
        const CommandLine base = line();
        m_services = std::make_unique<CodingServices>(base.options.agentDir, base.options.agentDir + "/catalog", true);
        m_factory = std::make_unique<CodingRuntimeFactory>(*m_services, base.options.startup);
    }

    ServeDependencies dependencies() {
        PlatformServices& platform = m_services->platform();
        ServeDependencies result;
        result.files = &platform.files();
        result.clock = &platform.clock();
        result.ids = &platform.ids();
        result.crypto = &platform.crypto();
        result.logger = &platform.logger();
        result.sessions = &m_services->sessions();
        result.runtimes = m_factory.get();
        result.models = &m_services->models().models();
        return result;
    }

    ~ServeApplicationTest() override {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_app.reset();
        std::filesystem::remove_all(m_serverDir);
    }

    CommandLine line() const {
        CommandLine result;
        result.command = "serve";
        result.options.cwd = m_dir + "/project";
        result.options.agentDir = m_dir + "/agent";
        result.options.sessionDir = m_dir + "/sessions";
        result.options.faux = true;
        result.options.startup.model = "faux/faux-1";
        result.options.startup.noMcp = true;
        result.options.startup.noPlugins = true;
        result.options.startup.noContextFiles = true;
        result.options.startup.noSkills = true;
        result.options.startup.noPromptTemplates = true;
        result.options.startup.noTools = true;
        result.serverDir = m_serverDir;
        result.serverId = std::string(kServerId);
        return result;
    }

    /** The opener of the durable backend; nothing for the session-tree one. */
    std::shared_ptr<ISessionOpener> opener() {
        if (!GetParam()) {
            return nullptr;
        }
        const CommandLine base = line();
        m_durable = std::make_unique<DurableServe>(*m_services, base.options.startup, base.options.agentDir);
        return m_durable->opener();
    }

    void startServer() {
        m_app = std::make_unique<ServeApplication>(line(), dependencies(), opener());
        const auto started = m_app->start();
        ASSERT_TRUE(started) << started.error().message;
    }

    bool dial() {
        m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un target{};
        target.sun_family = AF_UNIX;
        std::strncpy(target.sun_path, m_app->socketPath().c_str(), sizeof(target.sun_path) - 1);
        return ::connect(m_fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0;
    }

    void send(const Json& message) {
        auto frame = m_codec.encode(ProtocolSide::Client, message);
        ASSERT_TRUE(frame) << frame.error().message;
        ASSERT_EQ(::send(m_fd, frame->data(), frame->size(), 0), static_cast<ssize_t>(frame->size()));
    }

    /** Reads until `count` server messages arrived in total (or a timeout). */
    bool receive(std::size_t count) {
        while (m_messages.size() < count) {
            pollfd waiting{m_fd, POLLIN, 0};
            if (::poll(&waiting, 1, 5000) <= 0) {
                return false;
            }
            std::array<char, 65536> buffer{};
            const ssize_t n = ::recv(m_fd, buffer.data(), buffer.size(), 0);
            if (n <= 0) {
                return false;
            }
            auto decoded = m_decoder.push(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
            if (!decoded) {
                return false;
            }
            for (Json& message : *decoded) {
                m_messages.push_back(std::move(message));
            }
        }
        return true;
    }

    Json request(const std::string& id, const std::string& member, const Json& args, const std::string& service,
                 const Json& target = Json()) {
        send(Json{{"type", "request"},
                  {"id", id},
                  {"target", target.is_null() ? Json{{"serverId", kServerId}} : target},
                  {"call", Json{{"serviceId", service}, {"member", member}, {"args", args}}}});
        return Json();
    }

    /** The response with this id among the messages received so far, waiting for it if needed. */
    Json responseFor(const std::string& id) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            for (const Json& message : m_messages) {
                if (message.value("type", "") == "response" && message["id"] == id) {
                    return message;
                }
            }
            if (!receive(m_messages.size() + 1)) {
                break;
            }
        }
        return Json();
    }

    std::string m_dir;
    std::string m_serverDir;
    int m_fd = -1;
    ProtocolCodec m_codec;
    ProtocolMessageDecoder m_decoder{ProtocolSide::Server};
    std::vector<Json> m_messages;
    std::unique_ptr<CodingServices> m_services;
    std::unique_ptr<CodingRuntimeFactory> m_factory;
    std::unique_ptr<DurableServe> m_durable;
    std::unique_ptr<ServeApplication> m_app;
};

TEST_P(ServeApplicationTest, ListensOnTheServerSocketAndAnswersHello) {
    startServer();
    EXPECT_EQ(m_app->serverId(), kServerId);
    EXPECT_EQ(m_app->socketPath(), m_serverDir + "/" + kServerId + ".sock");
    ASSERT_TRUE(dial());
    send(Json{{"type", "hello"}, {"version", 8}});
    ASSERT_TRUE(receive(1));
    EXPECT_EQ(m_messages[0]["type"], "hello");
    EXPECT_EQ(m_messages[0]["serverId"], kServerId);
}

TEST_P(ServeApplicationTest, CreatesAttachesAndPromptsASession) {
    startServer();
    auto* faux = m_services->models().faux();
    ASSERT_TRUE(faux != nullptr);
    faux->enqueue(faux->textResponse("pong"));
    ASSERT_TRUE(dial());
    send(Json{{"type", "hello"}, {"version", 8}});
    ASSERT_TRUE(receive(1));

    request("c", "create", Json::array({Json{{"id", "demo"}}}), "pi.session-management");
    const Json created = responseFor("c");
    ASSERT_EQ(created["ok"], true) << created.dump();
    EXPECT_EQ(created["result"]["sessionId"], "demo");

    request("a", "attach", Json::array({"demo"}), "pi.session-management");
    const Json attached = responseFor("a");
    ASSERT_EQ(attached["ok"], true) << attached.dump();
    Json target;
    for (const Json& message : m_messages) {
        if (message.value("type", "") == "attachment" && !message["attachment"].is_null()) {
            target = message["attachment"];
        }
    }
    ASSERT_TRUE(target.is_object());
    EXPECT_EQ(target["sessionId"], "demo");

    request("p", "prompt", Json::array({Json{{"message", "ping"}, {"images", nullptr}}}), "pi.agent-controller", target);
    const Json prompted = responseFor("p");
    ASSERT_EQ(prompted["ok"], true) << prompted.dump();
    ASSERT_EQ(prompted["result"]["accepted"], true) << prompted.dump();

    request("w", "waitForPrompt", Json::array({prompted["result"]["operationId"]}), "pi.agent-controller", target);
    const Json answer = responseFor("w");
    ASSERT_EQ(answer["ok"], true) << answer.dump();
    EXPECT_EQ(answer["result"]["status"], "done");
    EXPECT_EQ(answer["result"]["text"], "pong");
}

TEST_P(ServeApplicationTest, SessionsPersistAcrossServerRestarts) {
    startServer();
    ASSERT_TRUE(dial());
    send(Json{{"type", "hello"}, {"version", 8}});
    ASSERT_TRUE(receive(1));
    request("c", "create", Json::array({Json{{"id", "keep"}}}), "pi.session-management");
    ASSERT_EQ(responseFor("c")["ok"], true);
    ::close(m_fd);
    m_fd = -1;
    m_messages.clear();
    m_decoder = ProtocolMessageDecoder(ProtocolSide::Server);
    m_app.reset();
    EXPECT_FALSE(std::filesystem::exists(m_serverDir + "/" + kServerId + ".sock"));

    startServer();
    ASSERT_TRUE(dial());
    send(Json{{"type", "hello"}, {"version", 8}});
    ASSERT_TRUE(receive(1));
    request("s", "subscribe", Json::array({"dir", "pi.session-directory", "singleton"}), "$chord.service");
    const Json subscribed = responseFor("s");
    ASSERT_EQ(subscribed["ok"], true) << subscribed.dump();
    const std::string dump = subscribed["result"].dump();
    EXPECT_NE(dump.find("keep"), std::string::npos);
}

TEST_P(ServeApplicationTest, ADefaultServerIdentityIsCreatedAndKept) {
    CommandLine withoutId = line();
    withoutId.serverId.reset();
    {
        ServeApplication first(withoutId, dependencies(), opener());
        ASSERT_TRUE(first.start());
        EXPECT_EQ(first.serverId().size(), 36u);
        const std::string id = first.serverId();
        first.stop();
        ServeApplication second(withoutId, dependencies(), opener());
        ASSERT_TRUE(second.start());
        EXPECT_EQ(second.serverId(), id);
    }
}

TEST_P(ServeApplicationTest, ASecondServerOnTheSameSocketIsRefused) {
    startServer();
    ServeApplication second(line(), dependencies(), opener());
    const auto started = second.start();
    ASSERT_FALSE(started);
    EXPECT_NE(started.error().message.find("already running"), std::string::npos);
}

TEST_P(ServeApplicationTest, AProtocolClientDrivesASessionOverTheSocket) {
    startServer();
    auto* faux = m_services->models().faux();
    ASSERT_TRUE(faux != nullptr);
    faux->enqueue(faux->textResponse("pong"));
    PosixUnixConnector connector(m_app->socketPath(), [](int fd, std::uint64_t limit, std::int64_t grace) {
        return std::shared_ptr<ISocketConnection>(std::make_shared<PosixByteConnection>(fd, limit, grace));
    });
    ProtocolClient client(connector, kServerId);
    const auto hello = client.connect();
    ASSERT_TRUE(hello.has_value()) << hello.error().message;
    EXPECT_EQ(hello->at("serverId"), kServerId);

    const auto catalogue = client.serviceCatalogue(client.serverTarget());
    ASSERT_TRUE(catalogue.has_value()) << catalogue.error().message;
    EXPECT_NE(catalogue->dump().find("pi.session-management"), std::string::npos);

    const auto call = [](const std::string& service, const std::string& member, const Json& args) {
        return Json{{"serviceId", service}, {"member", member}, {"args", args}};
    };
    const auto created = client.request(client.serverTarget(), call("pi.session-management", "create", Json::array({Json{{"id", "demo"}}})));
    ASSERT_TRUE(created.has_value()) << created.error().message;
    ASSERT_TRUE(client.request(client.serverTarget(), call("pi.session-management", "attach", Json::array({"demo"}))).has_value());
    std::optional<Json> target;
    for (int i = 0; i < 500 && !(target = client.attachment()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target->at("sessionId"), "demo");

    std::mutex mutex;
    std::string transcript;
    const auto subscription = client.subscribeService(*target, "pi.transcript", "singleton", [&](const Json& update) {
        const std::lock_guard<std::mutex> lock(mutex);
        transcript += update.dump();
    });
    ASSERT_TRUE(subscription.has_value()) << subscription.error().message;
    client.start(subscription->id);

    const auto prompted = client.request(*target, call("pi.agent-controller", "prompt", Json::array({Json{{"message", "ping"}, {"images", nullptr}}})));
    ASSERT_TRUE(prompted.has_value() && prompted->has_value()) << (prompted ? "" : prompted.error().message);
    ASSERT_EQ((**prompted)["accepted"], true);
    const auto answer = client.request(*target, call("pi.agent-controller", "waitForPrompt", Json::array({(**prompted)["operationId"]})));
    ASSERT_TRUE(answer.has_value() && answer->has_value()) << (answer ? "" : answer.error().message);
    EXPECT_EQ((**answer)["text"], "pong");

    bool streamed = false;
    for (int i = 0; i < 500 && !streamed; ++i) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            streamed = transcript.find("pong") != std::string::npos;
        }
        if (!streamed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(streamed);
    ASSERT_TRUE(client.dispose(subscription->id).has_value());
    client.disconnect();
}

TEST_P(ServeApplicationTest, AProtocolClientRefusesAServerWithAnotherId) {
    startServer();
    PosixUnixConnector connector(m_app->socketPath(), [](int fd, std::uint64_t limit, std::int64_t grace) {
        return std::shared_ptr<ISocketConnection>(std::make_shared<PosixByteConnection>(fd, limit, grace));
    });
    ProtocolClient client(connector, "00000000-0000-4000-8000-000000000002");
    const auto hello = client.connect();
    ASSERT_FALSE(hello.has_value());
    EXPECT_EQ(hello.error().code, "protocol_validation");
}

INSTANTIATE_TEST_SUITE_P(Backends, ServeApplicationTest, testing::Bool(), [](const testing::TestParamInfo<bool>& info) { return info.param ? std::string("Durable") : std::string("SessionTree"); });
