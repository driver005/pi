#include <gtest/gtest.h>

import std;
import pi.support.invocation_runtime;

class RecordingAgent : public IAgent {
public:
    std::shared_ptr<const AgentSnapshot> snapshot() const override {
        return std::make_shared<AgentSnapshot>();
    }

    const std::vector<PromptSection>& sections() const override {
        return m_sections;
    }

    std::vector<HookRegistration> hooks(const std::string& taskName) const override {
        std::vector<HookRegistration> found;
        for (const HookRegistration& registration : m_registrations) {
            if (registration.task == taskName) {
                found.push_back(registration);
            }
        }
        return found;
    }

    void add(HookRegistration registration) {
        m_registrations.push_back(std::move(registration));
    }

private:
    std::vector<PromptSection> m_sections;
    std::vector<HookRegistration> m_registrations;
};

class RecordingHost : public ITaskHost {
public:
    Result<void> commit(TaskInvocation&, const std::function<Result<std::optional<Json>>(Transaction&, const Json&)>&) override {
        ++m_commits;
        return {};
    }
    Result<std::optional<Json>> memo(TaskInvocation&, const std::string&) override {
        return std::optional<Json>();
    }
    Result<Json> memo(TaskInvocation&, const std::string&, const Json& candidate) override {
        return candidate;
    }
    Result<std::optional<Json>> snapshot(TaskInvocation&, const DocDefinition&, const DocAddressArgs&) override {
        return std::optional<Json>();
    }
    Result<std::optional<Json>> snapshotAsOf(TaskInvocation&, const DocDefinition&, const DocAddressArgs&, std::int64_t) override {
        return std::optional<Json>();
    }
    Result<std::optional<Json>> getTask(TaskInvocation&, std::int64_t) override {
        return std::optional<Json>();
    }
    Result<Json> waitForTask(TaskInvocation&, std::int64_t, const AbortSignal*) override {
        return Json::object();
    }
    Result<std::vector<Json>> outcomes(TaskInvocation&, const std::vector<std::int64_t>&) override {
        return std::vector<Json>();
    }
    Result<std::shared_ptr<IConversationHandle>> conversation(const std::shared_ptr<TaskInvocation>&, std::int64_t) override {
        return std::shared_ptr<IConversationHandle>();
    }
    Result<std::optional<Json>> entry(TaskInvocation&, std::int64_t, const std::optional<std::string>&) override {
        return std::optional<Json>();
    }
    Result<Json> context(TaskInvocation&, std::int64_t, const std::optional<std::int64_t>&) override {
        return Json::object();
    }
    Result<std::shared_ptr<const IAgent>> resolveAgent(TaskInvocation&, const std::shared_ptr<const IRegistrySnapshot>&) override {
        ++m_resolutions;
        return std::shared_ptr<const IAgent>(m_agent);
    }
    ResolvedSettings settings() override {
        return ResolvedSettings{};
    }
    IModelRuntime* models() override {
        return nullptr;
    }
    Result<std::shared_ptr<IExecutionEnv>> env(TaskInvocation&) override {
        return std::shared_ptr<IExecutionEnv>();
    }
    std::int64_t now(TaskInvocation&) override {
        return 42;
    }
    void report(TaskInvocation&, const Error& error) override {
        m_reports.push_back(error.message);
    }
    Result<void> sleep(TaskInvocation&, std::int64_t, const AbortSignal*) override {
        return {};
    }

    std::shared_ptr<RecordingAgent> m_agent = std::make_shared<RecordingAgent>();
    int m_resolutions = 0;
    int m_commits = 0;
    std::vector<std::string> m_reports;
};

class InvocationRuntimeTest : public ::testing::Test {
protected:
    InvocationRuntimeTest() : m_invocation(std::make_shared<TaskInvocation>()) {
        m_invocation->taskId = 9;
        m_invocation->conversationId = 4;
    }

    InvocationRuntime runtime() {
        return InvocationRuntime(m_host, m_invocation, "pi.test", [] { return std::shared_ptr<const IRegistrySnapshot>(); });
    }

    RecordingHost m_host;
    std::shared_ptr<TaskInvocation> m_invocation;
};

TEST_F(InvocationRuntimeTest, IdentityAndDelegation) {
    InvocationRuntime rt = runtime();
    EXPECT_EQ(rt.taskId(), 9);
    EXPECT_EQ(rt.conversationId(), 4);
    EXPECT_EQ(rt.now(), 42);
    ASSERT_TRUE(rt.commit([](Transaction&, const Json&) -> Result<std::optional<Json>> { return std::optional<Json>(); }).has_value());
    EXPECT_EQ(m_host.m_commits, 1);
}

TEST_F(InvocationRuntimeTest, AgentIsResolvedOncePerPhase) {
    InvocationRuntime rt = runtime();
    ASSERT_TRUE(rt.agent().has_value());
    ASSERT_TRUE(rt.agent().has_value());
    EXPECT_EQ(m_host.m_resolutions, 1);
    rt.resetPhase();
    ASSERT_TRUE(rt.agent().has_value());
    EXPECT_EQ(m_host.m_resolutions, 2);
}

TEST_F(InvocationRuntimeTest, AgentFailsOnceTheInvocationEnded) {
    InvocationRuntime rt = runtime();
    m_invocation->ended = true;
    auto agent = rt.agent();
    ASSERT_FALSE(agent.has_value());
    EXPECT_EQ(agent.error().code, "invocation_ended");
}

TEST_F(InvocationRuntimeTest, HookFailuresAreReportedAndTheNextHandlerRuns) {
    HookRegistration first;
    first.task = "pi.test";
    first.handlers["beforeThing"] = [](const Json&, IHookApi&) -> Result<std::optional<Json>> {
        return std::unexpected(Error{"boom", "first failed"});
    };
    HookRegistration second;
    second.task = "pi.test";
    second.handlers["beforeThing"] = [](const Json&, IHookApi&) -> Result<std::optional<Json>> { return std::optional<Json>(Json(1)); };
    HookRegistration other;
    other.task = "pi.other";
    other.handlers["beforeThing"] = [](const Json&, IHookApi&) -> Result<std::optional<Json>> { return std::optional<Json>(Json(2)); };
    m_host.m_agent->add(first);
    m_host.m_agent->add(other);
    m_host.m_agent->add(second);
    InvocationRuntime rt = runtime();
    std::vector<int> seen;
    auto each = rt.eachHook("beforeThing", [&](const HookHandler& handler) -> Result<void> {
        auto decision = handler(Json::object(), rt);
        if (!decision) {
            return std::unexpected(decision.error());
        }
        seen.push_back((*decision)->get<int>());
        return {};
    });
    ASSERT_TRUE(each.has_value());
    EXPECT_EQ(seen, std::vector<int>({1}));
    ASSERT_EQ(m_host.m_reports.size(), 1u);
    EXPECT_EQ(m_host.m_reports[0], "first failed");
}

TEST_F(InvocationRuntimeTest, HookFailureAfterAbortPropagates) {
    HookRegistration registration;
    registration.task = "pi.test";
    registration.handlers["h"] = [](const Json&, IHookApi&) -> Result<std::optional<Json>> { return std::optional<Json>(); };
    m_host.m_agent->add(registration);
    InvocationRuntime rt = runtime();
    m_invocation->signal.abort();
    auto each = rt.eachHook("h", [](const HookHandler&) -> Result<void> { return std::unexpected(Error{"boom", "x"}); });
    ASSERT_FALSE(each.has_value());
    EXPECT_TRUE(m_host.m_reports.empty());
}
