#include <gtest/gtest.h>

import std;
import pi.support.durable_transcript_service;
import pi.testing.durable_harness_fixture;

TEST(DurableTranscriptServiceTest, ServesTheConversationViewAndFollowsARun) {
    DurableHarnessFixture fixture;
    ASSERT_TRUE(fixture.open().has_value());
    auto view = fixture.harness().viewState(fixture.root()->id());
    ASSERT_TRUE(view.has_value());
    DurableTranscriptService service(*view);
    EXPECT_TRUE(service.methods().empty());
    ASSERT_EQ(service.states().size(), 1u);
    IReplicatedState& state = *service.states().at("state");
    EXPECT_EQ(state.snapshot().value.at("entries").size(), 0u);
    EXPECT_TRUE(state.snapshot().value.at("docs").contains("pi.agent"));
    fixture.faux().enqueue(fixture.faux().textResponse("hello"));
    auto submission = fixture.root()->submit(fixture.input("hi"));
    ASSERT_TRUE(submission.has_value() && (*submission)->wait().has_value());
    ASSERT_TRUE(fixture.root()->waitForIdle().has_value());
    // Publications are delivered after the commit, so the view may trail the run by a moment.
    for (int attempt = 0; attempt < 1000 && state.snapshot().value.at("entries").size() < 2u; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(state.snapshot().value.at("entries").size(), 2u);
    EXPECT_EQ(state.snapshot().value.at("entries")[1].at("kind"), "pi.assistant");
}
