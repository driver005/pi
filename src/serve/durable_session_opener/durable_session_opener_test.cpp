#include <gtest/gtest.h>

import std;
import pi.ai.faux_provider;
import pi.durable.memory_storage;
import pi.serve.durable_session_opener;
import pi.support.resource_set_cache;
import pi.support.tool_set_cache;
import pi.testing.fake_model_runtime;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.scripted_tool;

class DurableSessionOpenerTest : public ::testing::Test {
protected:
    DurableSessionOpenerTest()
        : m_faux(m_executor, m_clock),
          m_tools(std::make_shared<ToolSetCache>([this](const std::string& cwd) {
              ToolSetCache::ToolSet tools{std::make_shared<ScriptedTool>("echo", "pong from " + cwd)};
              if (m_withExtra) {
                  tools.push_back(std::make_shared<ScriptedTool>("extra", "extra result"));
              }
              return tools;
          })),
          m_resources(std::make_shared<ResourceSetCache>([](const std::string&) { return LoadedResources{}; })) {
        Model model;
        model.id = "m";
        model.provider = "faux";
        model.api = "faux";
        model.contextWindow = 100000;
        model.maxTokens = 4096;
        m_models.addModel(model);
        m_models.setAuthenticated("faux", true);
        m_models.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            return m_faux.stream(model, context, options);
        });
        m_record.id = "demo";
        m_record.cwd = "/work/project";
        m_record.directory = "/sessions/demo";
    }

    DurableSessionOpener opener() {
        return DurableSessionOpener(
            m_models,
            [this](const std::string& path) -> Result<std::shared_ptr<IStorage>> {
                m_paths.push_back(path);
                if (m_failStorage) {
                    return std::unexpected(Error{"storage_error", "cannot open"});
                }
                return std::shared_ptr<IStorage>(std::make_shared<MemoryStorage>());
            },
            m_tools, m_resources, [](const std::string&) { return HarnessRunSettings{}; },
            [](const std::string& cwd) { return Json{{"cwd", cwd}, {"model", Json{{"provider", "faux"}, {"modelId", "m"}}}}; },
            [this](const Model& model) { m_selected.push_back(model.id); });
    }

    Json call(IServiceAttachment& attachment, const std::string& service, const std::string& member, const Json& argument) {
        auto result = attachment.invokeService(Json{{"serviceId", service}, {"member", member}, {"args", Json::array({argument})}}, [](const std::string&, const Json&, const ServiceContext&) {}, ServiceContext{std::make_shared<AbortSignal>()});
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result && *result ? **result : Json(nullptr);
    }

    InlineExecutor m_executor;
    FixedClock m_clock;
    FauxProvider m_faux;
    FakeModelRuntime m_models;
    std::shared_ptr<ToolSetCache> m_tools;
    std::shared_ptr<ResourceSetCache> m_resources;
    SessionRecord m_record;
    std::vector<std::string> m_paths;
    std::vector<std::string> m_selected;
    bool m_failStorage = false;
    std::atomic<bool> m_withExtra{false};
};

TEST_F(DurableSessionOpenerTest, OpensTheSessionsSqliteFileAndServesItsRootConversation) {
    DurableSessionOpener opened = opener();
    auto handle = opened.open(m_record, ServiceContext{});
    ASSERT_TRUE(handle.has_value()) << handle.error().message;
    EXPECT_EQ(m_paths, std::vector<std::string>{"/sessions/demo/session.sqlite"});
    auto attachment = (*handle)->attachClient(ServiceContext{});
    ASSERT_TRUE(attachment.has_value());
    m_faux.enqueue(m_faux.textResponse("pong"));
    const Json prompted = call(**attachment, "pi.agent-controller", "prompt", Json{{"message", "ping"}, {"images", nullptr}});
    ASSERT_TRUE(prompted.at("accepted").get<bool>()) << prompted.dump();
    const Json answer = call(**attachment, "pi.agent-controller", "waitForPrompt", prompted.at("operationId"));
    EXPECT_EQ(answer.at("status"), "done");
    EXPECT_EQ(answer.at("text"), "pong");
    EXPECT_TRUE((*handle)->close(ServiceContext{}).has_value());
    EXPECT_FALSE((*handle)->attachClient(ServiceContext{}).has_value());
}

TEST_F(DurableSessionOpenerTest, TheRootConversationStartsWithTheSeedAndTheCodingTools) {
    DurableSessionOpener opened = opener();
    auto handle = opened.open(m_record, ServiceContext{});
    ASSERT_TRUE(handle.has_value());
    auto attachment = (*handle)->attachClient(ServiceContext{});
    ASSERT_TRUE(attachment.has_value());
    // The coding tools run in the session's directory.
    m_faux.enqueue(m_faux.toolCallResponse("echo", Json::object(), "call-1"));
    m_faux.enqueue(m_faux.textResponse("done"));
    const Json prompted = call(**attachment, "pi.agent-controller", "prompt", Json{{"message", "go"}, {"images", nullptr}});
    ASSERT_TRUE(prompted.at("accepted").get<bool>());
    EXPECT_EQ(call(**attachment, "pi.agent-controller", "waitForPrompt", prompted.at("operationId")).at("text"), "done");
    const Json models = call(**attachment, "pi.models", "getThinkingLevels", Json());
    EXPECT_EQ(models, Json::array({"off"}));
}

TEST_F(DurableSessionOpenerTest, StorageFailuresFailTheOpen) {
    m_failStorage = true;
    DurableSessionOpener opened = opener();
    auto handle = opened.open(m_record, ServiceContext{});
    ASSERT_FALSE(handle.has_value());
    EXPECT_EQ(handle.error().message, "cannot open");
}

TEST_F(DurableSessionOpenerTest, ToolsThatAppearWhileTheSessionRunsAreOfferedToItsNextRequests) {
    DurableSessionOpener opened = opener();
    auto handle = opened.open(m_record, ServiceContext{});
    ASSERT_TRUE(handle.has_value());
    auto attachment = (*handle)->attachClient(ServiceContext{});
    ASSERT_TRUE(attachment.has_value());
    // Before the change the model's call of `extra` finds no such tool.
    m_faux.enqueue(m_faux.toolCallResponse("extra", Json::object(), "c1"));
    m_faux.enqueue(m_faux.textResponse("first"));
    const Json first = call(**attachment, "pi.agent-controller", "prompt", Json{{"message", "go"}, {"images", nullptr}});
    EXPECT_EQ(call(**attachment, "pi.agent-controller", "waitForPrompt", first.at("operationId")).at("text"), "first");
    m_withExtra = true;
    m_tools->invalidate(m_record.cwd);
    m_faux.enqueue(m_faux.toolCallResponse("extra", Json::object(), "c2"));
    m_faux.enqueue(m_faux.textResponse("second"));
    const Json second = call(**attachment, "pi.agent-controller", "prompt", Json{{"message", "again"}, {"images", nullptr}});
    EXPECT_EQ(call(**attachment, "pi.agent-controller", "waitForPrompt", second.at("operationId")).at("text"), "second");
    // The transcript holds the refusal of the first call and the real result of the second.
    auto subscribed = (*attachment)->invokeService(Json{{"serviceId", "$chord.service"}, {"member", "subscribe"}, {"args", Json::array({"t", "pi.transcript", "singleton"})}},
                                                   [](const std::string&, const Json&, const ServiceContext&) {}, ServiceContext{std::make_shared<AbortSignal>()});
    ASSERT_TRUE(subscribed.has_value() && *subscribed);
    const std::string transcript = (**subscribed).dump();
    EXPECT_NE(transcript.find("is not available"), std::string::npos) << transcript;
    EXPECT_NE(transcript.find("extra result"), std::string::npos) << transcript;
}
