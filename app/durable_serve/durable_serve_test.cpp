#include <gtest/gtest.h>

#include <cstdlib>

import std;
import pi.coding_services;
import pi.durable_serve;

class DurableServeTest : public testing::Test {
protected:
    DurableServeTest() {
        const std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/durable_serve_" + name;
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/project");
        m_startup.model = "faux/faux-1";
        m_startup.noMcp = true;
        m_startup.noPlugins = true;
        m_startup.noContextFiles = true;
        m_startup.noSkills = true;
        m_startup.noPromptTemplates = true;
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
    }

    SessionRecord record() const {
        SessionRecord record;
        record.id = "demo";
        record.cwd = m_dir + "/project";
        record.directory = m_dir + "/sessions/demo";
        return record;
    }

    Json call(IServiceAttachment& attachment, const std::string& service, const std::string& member, const Json& argument) {
        return callWith(attachment, service, member, argument.is_null() ? Json::array() : Json::array({argument}));
    }

    Json callWith(IServiceAttachment& attachment, const std::string& service, const std::string& member, const Json& args) {
        auto result = attachment.invokeService(Json{{"serviceId", service}, {"member", member}, {"args", args}}, [](const std::string&, const Json&, const ServiceContext&) {}, ServiceContext{std::make_shared<AbortSignal>()});
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result && *result ? **result : Json(nullptr);
    }

    std::string m_dir;
    CodingStartupOptions m_startup;
    std::unique_ptr<CodingServices> m_services;
};

TEST_F(DurableServeTest, OpensASqliteSessionRunsAPromptAndKeepsItsHistoryAcrossReopening) {
    DurableServe durable(*m_services, m_startup, m_dir + "/agent");
    const std::shared_ptr<ISessionOpener> opener = durable.opener();
    auto* faux = m_services->models().faux();
    ASSERT_TRUE(faux != nullptr);
    {
        auto handle = opener->open(record(), ServiceContext{});
        ASSERT_TRUE(handle.has_value()) << handle.error().message;
        EXPECT_TRUE(std::filesystem::exists(m_dir + "/sessions/demo/session.sqlite"));
        auto attachment = (*handle)->attachClient(ServiceContext{});
        ASSERT_TRUE(attachment.has_value());
        faux->enqueue(faux->textResponse("pong"));
        const Json prompted = call(**attachment, "pi.agent-controller", "prompt", Json{{"message", "ping"}, {"images", nullptr}});
        ASSERT_TRUE(prompted.at("accepted").get<bool>()) << prompted.dump();
        const Json answer = call(**attachment, "pi.agent-controller", "waitForPrompt", prompted.at("operationId"));
        EXPECT_EQ(answer.at("status"), "done");
        EXPECT_EQ(answer.at("text"), "pong");
        ASSERT_TRUE((*handle)->close(ServiceContext{}).has_value());
    }
    auto reopened = opener->open(record(), ServiceContext{});
    ASSERT_TRUE(reopened.has_value()) << reopened.error().message;
    auto attachment = (*reopened)->attachClient(ServiceContext{});
    ASSERT_TRUE(attachment.has_value());
    // The reopened session shows the transcript of the first run: the input and the answer.
    const Json subscribed = callWith(**attachment, "$chord.service", "subscribe", Json::array({"t", "pi.transcript", "singleton"}));
    const std::string dump = subscribed.dump();
    EXPECT_NE(dump.find("pi.assistant"), std::string::npos) << dump;
    EXPECT_NE(dump.find("pong"), std::string::npos) << dump;
}

TEST_F(DurableServeTest, ANewSessionStartsWithTheStartupModel) {
    DurableServe durable(*m_services, m_startup, m_dir + "/agent");
    auto handle = durable.opener()->open(record(), ServiceContext{});
    ASSERT_TRUE(handle.has_value()) << handle.error().message;
    auto attachment = (*handle)->attachClient(ServiceContext{});
    ASSERT_TRUE(attachment.has_value());
    const Json levels = call(**attachment, "pi.models", "getThinkingLevels", Json());
    EXPECT_TRUE(levels.is_array());
}

TEST_F(DurableServeTest, PluginToolsAreOfferedToDurableSessions) {
    m_startup.pluginPaths = {"plugins/hello_tool/libhello_tool.so"};
    m_startup.noPlugins = false;
    DurableServe durable(*m_services, m_startup, m_dir + "/agent");
    auto* faux = m_services->models().faux();
    faux->enqueue(faux->toolCallResponse("hello", Json{{"name", "Ada"}}, "c1"));
    faux->enqueue(faux->textResponse("done"));
    auto handle = durable.opener()->open(record(), ServiceContext{});
    ASSERT_TRUE(handle.has_value()) << handle.error().message;
    auto attachment = (*handle)->attachClient(ServiceContext{});
    ASSERT_TRUE(attachment.has_value());
    const Json prompted = call(**attachment, "pi.agent-controller", "prompt", Json{{"message", "greet"}, {"images", nullptr}});
    ASSERT_TRUE(prompted.at("accepted").get<bool>());
    EXPECT_EQ(call(**attachment, "pi.agent-controller", "waitForPrompt", prompted.at("operationId")).at("text"), "done");
    const Json subscribed = callWith(**attachment, "$chord.service", "subscribe", Json::array({"t", "pi.transcript", "singleton"}));
    EXPECT_NE(subscribed.dump().find("Hello, Ada!"), std::string::npos) << subscribed.dump();
}

TEST_F(DurableServeTest, PluginHooksGuardTheToolCallsOfDurableSessions) {
    m_startup.pluginPaths = {"plugins/hello_tool/libhello_tool.so"};
    m_startup.noPlugins = false;
    DurableServe durable(*m_services, m_startup, m_dir + "/agent");
    auto* faux = m_services->models().faux();
    // The hello plugin refuses `rm -rf` before bash would run it.
    faux->enqueue(faux->toolCallResponse("bash", Json{{"command", "rm -rf " + m_dir + "/project/victim"}}, "c1"));
    faux->enqueue(faux->textResponse("done"));
    std::filesystem::create_directories(m_dir + "/project/victim");
    auto handle = durable.opener()->open(record(), ServiceContext{});
    ASSERT_TRUE(handle.has_value()) << handle.error().message;
    auto attachment = (*handle)->attachClient(ServiceContext{});
    ASSERT_TRUE(attachment.has_value());
    const Json prompted = call(**attachment, "pi.agent-controller", "prompt", Json{{"message", "clean up"}, {"images", nullptr}});
    ASSERT_TRUE(prompted.at("accepted").get<bool>());
    EXPECT_EQ(call(**attachment, "pi.agent-controller", "waitForPrompt", prompted.at("operationId")).at("text"), "done");
    EXPECT_TRUE(std::filesystem::exists(m_dir + "/project/victim"));
    const Json subscribed = callWith(**attachment, "$chord.service", "subscribe", Json::array({"t", "pi.transcript", "singleton"}));
    EXPECT_NE(subscribed.dump().find("hello plugin refuses rm -rf"), std::string::npos) << subscribed.dump();
}
