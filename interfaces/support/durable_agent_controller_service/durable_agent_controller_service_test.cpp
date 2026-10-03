#include <gtest/gtest.h>

import std;
import pi.support.durable_agent_controller_service;
import pi.support.wait_gate;
import pi.testing.durable_harness_fixture;

class DurableAgentControllerServiceTest : public ::testing::Test {
protected:
    DurableAgentControllerServiceTest() {
        EXPECT_TRUE(m_fixture.open().has_value());
        m_service = std::make_unique<DurableAgentControllerService>(m_fixture.harness(), m_fixture.root());
        m_methods = m_service->methods();
    }

    Json call(const std::string& method, const Json& argument) {
        auto result = m_methods.at(method)({argument}, ServiceContext{std::make_shared<AbortSignal>()});
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result && *result ? **result : Json(nullptr);
    }

    Json prompt(const std::string& message) {
        return call("prompt", Json{{"message", message}, {"images", nullptr}});
    }

    DurableHarnessFixture m_fixture;
    std::unique_ptr<DurableAgentControllerService> m_service;
    std::map<std::string, IRemoteService::Method> m_methods;
};

TEST_F(DurableAgentControllerServiceTest, PromptRunsAndWaitForPromptReturnsTheAnswerText) {
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("hello there"));
    const Json response = prompt("hi");
    ASSERT_TRUE(response.at("accepted").get<bool>());
    EXPECT_TRUE(response.at("error").is_null());
    const std::string id = response.at("operationId").get<std::string>();
    const Json answer = call("waitForPrompt", id);
    EXPECT_EQ(answer.at("status"), "done");
    EXPECT_EQ(answer.at("text"), "hello there");
    EXPECT_TRUE(answer.at("reason").is_null());
}

TEST_F(DurableAgentControllerServiceTest, ABusyConversationRejectsPromptsButQueuesSteeringAndFollowUps) {
    WaitGate streaming;
    WaitGate release;
    m_fixture.models().setStreamHandler([&](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        streaming.open();
        (void)release.wait({options.signal.get()});
        return m_fixture.faux().stream(model, context, options);
    });
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("first"));
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("second"));
    const Json first = prompt("one");
    ASSERT_TRUE(first.at("accepted").get<bool>());
    ASSERT_TRUE(streaming.wait({}).has_value());
    const Json rejected = prompt("two");
    EXPECT_FALSE(rejected.at("accepted").get<bool>());
    EXPECT_EQ(rejected.at("error").at("code"), "busy");
    EXPECT_TRUE(rejected.at("operationId").is_null());
    const Json queued = call("followUp", Json{{"message", "three"}, {"images", nullptr}});
    ASSERT_TRUE(queued.at("accepted").get<bool>());
    const Json steer = call("steer", Json{{"message", "four"}, {"images", nullptr}});
    ASSERT_TRUE(steer.at("accepted").get<bool>());
    // Withdrawing the queued follow-up before it is consumed.
    EXPECT_EQ(call("cancelQueued", queued.at("entryId")).at("outcome"), "cancelled");
    EXPECT_EQ(call("cancelQueued", queued.at("entryId")).at("outcome"), "already_consumed");
    EXPECT_EQ(call("cancelQueued", "999999").at("outcome"), "not_found");
    EXPECT_EQ(call("cancelQueued", "not-a-number").at("outcome"), "not_found");
    release.open();
    EXPECT_EQ(call("waitForPrompt", first.at("operationId")).at("status"), "done");
}

TEST_F(DurableAgentControllerServiceTest, AbortSettlesTheRunUnanswered) {
    WaitGate streaming;
    m_fixture.models().setStreamHandler([&](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        streaming.open();
        (void)WaitGate().wait({options.signal.get()});
        return m_fixture.faux().stream(model, context, options);
    });
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("never"));
    const Json started = prompt("go");
    ASSERT_TRUE(started.at("accepted").get<bool>());
    ASSERT_TRUE(streaming.wait({}).has_value());
    call("abort", Json());
    const Json answer = call("waitForPrompt", started.at("operationId"));
    EXPECT_EQ(answer.at("status"), "unanswered");
    EXPECT_EQ(answer.at("reason"), "aborted");
    EXPECT_TRUE(answer.at("text").is_null());
}

TEST_F(DurableAgentControllerServiceTest, CompactReturnsATaskIdAndImagesTravelAsContentBlocks) {
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("seen"));
    const Json response = call("prompt", Json{{"message", "look"}, {"images", Json::array({Json{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}}})}});
    ASSERT_TRUE(response.at("accepted").get<bool>());
    EXPECT_EQ(call("waitForPrompt", response.at("operationId")).at("text"), "seen");
    auto context = m_fixture.root()->context();
    ASSERT_TRUE(context.has_value());
    const Json user = context->at("messages")[0];
    ASSERT_TRUE(user.at("content").is_array());
    EXPECT_EQ(user.at("content")[1].at("mimeType"), "image/png");
    m_fixture.faux().enqueue(m_fixture.faux().textResponse("a summary"));
    const Json compacted = call("compact", Json{{"customInstructions", nullptr}});
    EXPECT_TRUE(compacted.at("accepted").get<bool>());
    EXPECT_FALSE(compacted.at("operationId").get<std::string>().empty());
}

TEST_F(DurableAgentControllerServiceTest, InvalidRequestsAndUnknownPromptsAreErrors) {
    auto invalid = m_methods.at("prompt")({Json{{"message", 5}}}, ServiceContext{});
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code, "invalid_request");
    EXPECT_FALSE(m_methods.at("prompt")({}, ServiceContext{}).has_value());
    EXPECT_FALSE(m_methods.at("compact")({Json("x")}, ServiceContext{}).has_value());
    auto unknown = m_methods.at("waitForPrompt")({Json("424242")}, ServiceContext{std::make_shared<AbortSignal>()});
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code, "operation_not_found");
    EXPECT_FALSE(m_methods.at("waitForPrompt")({Json("zero")}, ServiceContext{}).has_value());
}
