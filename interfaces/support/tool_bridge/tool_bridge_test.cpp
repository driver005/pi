#include <gtest/gtest.h>

import std;
import pi.ai.faux_provider;
import pi.support.directory_execution_env;
import pi.durable.memory_storage;
import pi.support.harness;
import pi.support.tool_bridge;
import pi.testing.fake_model_runtime;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.scripted_tool;

class ToolBridgeTest : public ::testing::Test {
protected:
    ToolBridgeTest()
        : m_registry(BuiltinTasks().all()),
          m_faux(m_executor, m_clock),
          m_cache(std::make_shared<ToolSetCache>([this](const std::string& cwd) {
              {
                  const std::lock_guard<std::mutex> lock(m_mutex);
                  m_built.push_back(cwd);
              }
              return ToolSetCache::ToolSet{std::make_shared<ScriptedTool>("echo", "pong from " + cwd)};
          })) {
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
        EXPECT_TRUE(m_registry.install(ToolBridge(m_cache, "/default").extension("coding-tools")).has_value());
    }

    ~ToolBridgeTest() override {
        if (m_harness) {
            (void)m_harness->close();
        }
    }

    void open() {
        HarnessOptions options;
        options.models = &m_models;
        options.registry = &m_registry;
        options.now = [] { return std::int64_t(1000); };
        options.env = [](std::int64_t, const std::optional<std::string>& cwd) -> Result<std::shared_ptr<IExecutionEnv>> {
            return std::shared_ptr<IExecutionEnv>(std::make_shared<DirectoryExecutionEnv>(cwd.value_or("/default")));
        };
        m_harness = std::make_unique<Harness>(std::make_shared<MemoryStorage>(), options);
        ASSERT_TRUE(m_harness->open().has_value());
    }

    std::shared_ptr<Conversation> conversationIn(const std::optional<std::string>& cwd) {
        ConversationCreateOptions create;
        create.agent = Json::object({{"model", Json::object({{"provider", "faux"}, {"modelId", "m"}})}});
        if (cwd) {
            (*create.agent)["cwd"] = *cwd;
        }
        auto root = m_harness->root(create);
        EXPECT_TRUE(root.has_value());
        return *root;
    }

    Json runWithToolCall(const std::shared_ptr<Conversation>& conversation) {
        m_faux.enqueue(m_faux.toolCallResponse("echo", Json::object({{"arg", "x"}}), "call-1"));
        m_faux.enqueue(m_faux.textResponse("done"));
        SubmissionDraft draft;
        draft.type = "input";
        draft.content = "go";
        auto submission = conversation->submit(draft);
        EXPECT_TRUE(submission.has_value());
        auto settled = (*submission)->wait();
        EXPECT_TRUE(settled.has_value());
        EXPECT_TRUE(conversation->waitForIdle().has_value());
        auto context = conversation->context();
        EXPECT_TRUE(context.has_value());
        return context ? *context : Json::object();
    }

    /** The first message with `role`; the context starts with a system entry listing the tools. */
    Json messageWithRole(const Json& context, const std::string& role) {
        for (const Json& message : context.at("messages")) {
            if (message.value("role", std::string()) == role) {
                return message;
            }
        }
        ADD_FAILURE() << "no " << role << " message";
        return Json::object();
    }

    std::vector<std::string> built() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_built;
    }

    InlineExecutor m_executor;
    Registry m_registry;
    FixedClock m_clock;
    FauxProvider m_faux;
    FakeModelRuntime m_models;
    std::mutex m_mutex;
    std::vector<std::string> m_built;
    std::shared_ptr<ToolSetCache> m_cache;
    std::unique_ptr<Harness> m_harness;
};

TEST_F(ToolBridgeTest, OffersEachToolWithItsOwnDeclaration) {
    const Extension extension = ToolBridge(m_cache, "/default").extension("coding-tools");
    EXPECT_EQ(extension.name, "coding-tools");
    ASSERT_EQ(extension.tools.size(), 1u);
    EXPECT_EQ(extension.tools[0].name, "echo");
    EXPECT_EQ(extension.tools[0].description, "scripted");
    EXPECT_TRUE(extension.tools[0].parameters.contains("properties"));
    EXPECT_FALSE(extension.tools[0].executionMode.has_value());
}

TEST_F(ToolBridgeTest, ARunCallsTheToolOfTheConversationsDirectory) {
    open();
    const Json context = runWithToolCall(conversationIn(std::string("/work/project")));
    const Json result = messageWithRole(context, "toolResult");
    EXPECT_EQ(result.value("toolCallId", std::string()), "call-1");
    EXPECT_EQ(result.at("content")[0].at("text"), "pong from /work/project");
    EXPECT_EQ(context.at("messages").back().at("content")[0].at("text"), "done");
    const std::vector<std::string> directories = built();
    EXPECT_NE(std::find(directories.begin(), directories.end(), "/work/project"), directories.end());
}

TEST_F(ToolBridgeTest, ConversationsWithoutADirectoryUseTheDefaultOne) {
    open();
    const Json context = runWithToolCall(conversationIn(std::nullopt));
    EXPECT_EQ(messageWithRole(context, "toolResult").at("content")[0].at("text"), "pong from /default");
}
