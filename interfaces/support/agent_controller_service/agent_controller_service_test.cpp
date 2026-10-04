#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.base.thread_pool;
import pi.support.agent_controller_service;
import pi.session.session_manager;
import pi.testing.sequential_id_generator;
import pi.testing.session_harness;
import pi.testing.test_session_handle;

class AgentControllerServiceTest : public ::testing::Test {
protected:
    AgentControllerServiceTest() {
        SessionManagerOptions options;
        options.cwd = "/work";
        options.persist = false;
        auto manager = std::make_unique<SessionManager>(options, m_harness.files(), m_harness.clock(), m_harness.ids());
        manager->open();
        SessionRuntimeRequest request;
        request.cwd = "/work";
        request.agentDir = "/agent";
        request.sessionManager = std::move(manager);
        m_handle = std::make_unique<TestSessionHandle>(std::move(request), m_harness.agents(), m_harness.provider(),
                                                       m_harness.files(), m_harness.clock(), m_harness.ids(),
                                                       m_harness.sleeper());
        m_service = std::make_unique<AgentControllerService>(m_handle->session(), m_executor, m_ids);
        m_methods = m_service->methods();
    }

    ~AgentControllerServiceTest() override {
        release();
        m_handle->session().abort();
        m_handle->session().waitForIdle();
    }

    Json call(const std::string& method, const Json& argument = Json()) {
        std::vector<Json> args;
        if (!argument.is_null()) {
            args.push_back(argument);
        }
        const ServiceContext context{std::make_shared<AbortSignal>()};
        auto result = m_methods.at(method)(args, context);
        EXPECT_TRUE(result) << (result ? "" : result.error().message);
        return result && result->has_value() ? **result : Json();
    }

    Json request(const std::string& message) const {
        return Json{{"message", message}, {"images", nullptr}};
    }

    /** The next scripted response blocks until release() or an abort. */
    void holdNextResponse(const std::string& text) {
        m_harness.provider().enqueue([this, text](const TranscriptContext&, const StreamOptions& options, const Model&) {
            std::unique_lock<std::mutex> lock(m_gate);
            m_started = true;
            m_gateChanged.notify_all();
            while (!m_released && !(options.signal && options.signal->aborted())) {
                m_gateChanged.wait_for(lock, std::chrono::milliseconds(5));
            }
            return m_harness.provider().textResponse(text);
        });
    }

    bool waitForStarted() {
        std::unique_lock<std::mutex> lock(m_gate);
        return m_gateChanged.wait_for(lock, std::chrono::seconds(3), [&] { return m_started; });
    }

    void release() {
        {
            const std::lock_guard<std::mutex> lock(m_gate);
            m_released = true;
        }
        m_gateChanged.notify_all();
    }

    SessionHarness m_harness{"/work"};
    SequentialIdGenerator m_ids{"op"};
    std::mutex m_gate;
    std::condition_variable m_gateChanged;
    bool m_started = false;
    bool m_released = false;
    std::unique_ptr<TestSessionHandle> m_handle;
    ThreadPool m_executor{4};
    std::unique_ptr<AgentControllerService> m_service;
    std::map<std::string, IRemoteService::Method> m_methods;
};

TEST_F(AgentControllerServiceTest, PromptStartsARunAndWaitForPromptReturnsTheAnswer) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("hello there"));
    const Json response = call("prompt", request("hi"));
    EXPECT_EQ(response["accepted"], true);
    EXPECT_EQ(response["error"], nullptr);
    const Json result = call("waitForPrompt", response["operationId"]);
    EXPECT_EQ(result["status"], "done");
    EXPECT_EQ(result["text"], "hello there");
    EXPECT_EQ(result["reason"], nullptr);
}

TEST_F(AgentControllerServiceTest, PromptIsRefusedWhileARunIsActive) {
    holdNextResponse("first");
    const Json first = call("prompt", request("one"));
    ASSERT_EQ(first["accepted"], true);
    ASSERT_TRUE(waitForStarted());
    const Json second = call("prompt", request("two"));
    EXPECT_EQ(second["accepted"], false);
    EXPECT_EQ(second["operationId"], nullptr);
    EXPECT_EQ(second["error"]["code"], "busy");
    release();
    EXPECT_EQ(call("waitForPrompt", first["operationId"])["text"], "first");
    m_harness.provider().enqueue(m_harness.provider().textResponse("again"));
    EXPECT_EQ(call("prompt", request("three"))["accepted"], true);
}

TEST_F(AgentControllerServiceTest, InvalidRequestsAreRejected) {
    const ServiceContext context{std::make_shared<AbortSignal>()};
    EXPECT_EQ(m_methods.at("prompt")({Json{{"message", 3}}}, context).error().code, "invalid_request");
    EXPECT_EQ(m_methods.at("prompt")({}, context).error().code, "invalid_request");
    EXPECT_EQ(m_methods.at("prompt")({Json{{"message", "x"}, {"images", Json::array({Json{{"data", 1}}})}}}, context).error().code,
              "invalid_request");
    EXPECT_EQ(m_methods.at("waitForPrompt")({Json("nope")}, context).error().code, "operation_not_found");
}

TEST_F(AgentControllerServiceTest, ImagesReachTheSession) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("seen"));
    const Json response =
        call("prompt", Json{{"message", "look"}, {"images", Json::array({Json{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}}})}});
    ASSERT_EQ(response["accepted"], true);
    EXPECT_EQ(call("waitForPrompt", response["operationId"])["status"], "done");
}

TEST_F(AgentControllerServiceTest, SteerStartsARunWhenIdle) {
    m_harness.provider().enqueue(m_harness.provider().textResponse("steered"));
    const Json response = call("steer", request("go"));
    EXPECT_EQ(response["accepted"], true);
    ASSERT_TRUE(response["entryId"].is_string());
    EXPECT_EQ(call("waitForPrompt", response["entryId"])["text"], "steered");
}

TEST_F(AgentControllerServiceTest, FollowUpQueuesBehindTheActiveRunAndCanBeWithdrawn) {
    holdNextResponse("busy");
    const Json run = call("prompt", request("run"));
    ASSERT_EQ(run["accepted"], true);
    ASSERT_TRUE(waitForStarted());
    const Json queued = call("followUp", request("later"));
    ASSERT_EQ(queued["accepted"], true);
    EXPECT_EQ(m_handle->session().queued().followUp, (std::vector<std::string>{"later"}));
    EXPECT_EQ(call("cancelQueued", queued["entryId"])["outcome"], "cancelled");
    EXPECT_TRUE(m_handle->session().queued().followUp.empty());
    EXPECT_EQ(call("cancelQueued", queued["entryId"])["outcome"], "not_found");
    release();
    call("waitForPrompt", run["operationId"]);
}

TEST_F(AgentControllerServiceTest, WithdrawingOneEntryKeepsTheOthers) {
    holdNextResponse("busy");
    const Json run = call("prompt", request("run"));
    ASSERT_TRUE(waitForStarted());
    const Json first = call("followUp", request("a"));
    const Json second = call("followUp", request("b"));
    const Json steer = call("steer", request("s"));
    ASSERT_EQ(first["accepted"], true);
    EXPECT_EQ(call("cancelQueued", first["entryId"])["outcome"], "cancelled");
    const QueuedInput left = m_handle->session().queued();
    EXPECT_EQ(left.followUp, (std::vector<std::string>{"b"}));
    EXPECT_EQ(left.steering, (std::vector<std::string>{"s"}));
    EXPECT_EQ(call("cancelQueued", second["entryId"])["outcome"], "cancelled");
    EXPECT_EQ(call("cancelQueued", steer["entryId"])["outcome"], "cancelled");
    release();
    call("waitForPrompt", run["operationId"]);
}

TEST_F(AgentControllerServiceTest, AbortWithdrawsQueuedInputAndStopsTheRun) {
    holdNextResponse("never");
    const Json run = call("prompt", request("run"));
    ASSERT_TRUE(waitForStarted());
    call("followUp", request("later"));
    call("abort");
    EXPECT_TRUE(m_handle->session().isIdle());
    EXPECT_TRUE(m_handle->session().queued().followUp.empty());
    const Json result = call("waitForPrompt", run["operationId"]);
    EXPECT_EQ(result["status"], "unanswered");
    EXPECT_EQ(result["reason"], "The run was aborted");
}

TEST_F(AgentControllerServiceTest, CompactionRunsAsAnOperation) {
    const Json response = call("compact", Json{{"customInstructions", nullptr}});
    EXPECT_EQ(response["accepted"], true);
    const Json result = call("waitForPrompt", response["operationId"]);
    EXPECT_TRUE(result["status"] == "done" || result["status"] == "unanswered");
}

TEST_F(AgentControllerServiceTest, WaitingEndsWhenTheCallIsCancelled) {
    holdNextResponse("slow");
    const Json run = call("prompt", request("run"));
    ASSERT_TRUE(waitForStarted());
    const auto signal = std::make_shared<AbortSignal>();
    std::thread canceller([signal] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        signal->abort();
    });
    const auto result = m_methods.at("waitForPrompt")({run["operationId"]}, ServiceContext{signal});
    canceller.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "cancelled");
    release();
    call("waitForPrompt", run["operationId"]);
}
