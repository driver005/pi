#include <gtest/gtest.h>

import std;
import pi.support.tool_task_definition;
import pi.testing.task_fixture;

class ToolTaskTest : public ::testing::Test {
protected:
    ToolTaskTest() : m_fixture({ToolTaskDefinition().build()}) {
        m_fixture.setAgentFactory([this](std::int64_t) { return agent(); });
    }

    std::shared_ptr<const IAgent> agent() {
        auto snapshot = std::make_shared<AgentSnapshot>();
        const std::lock_guard<std::mutex> lock(m_mutex);
        snapshot->tools = m_tools;
        auto extension = std::make_shared<Extension>();
        extension->name = "hooks";
        extension->hooks = m_hooks;
        return std::make_shared<ResolvedAgent>(snapshot, std::vector<PromptSection>{}, std::vector<std::shared_ptr<const Extension>>{extension});
    }

    ToolRegistration tool(const std::string& name, std::function<Result<ToolExecutionResult>(const Json&, IToolExecutionApi&)> execute) {
        ToolRegistration registration;
        registration.name = name;
        registration.description = "test tool";
        registration.parameters = Json::parse(R"({"type":"object","properties":{"text":{"type":"string"},"count":{"type":"number"}},"required":["text"]})");
        registration.execute = std::move(execute);
        return registration;
    }

    ToolExecutionResult textResult(const std::string& text) {
        ToolExecutionResult result;
        result.content = Json::array({Json::object({{"type", "text"}, {"text", text}})});
        return result;
    }

    void addTool(ToolRegistration registration) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_tools.push_back(std::move(registration));
    }

    void addHook(const std::string& name, HookHandler handler) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        HookRegistration registration;
        registration.task = "pi.tool";
        registration.handlers[name] = std::move(handler);
        m_hooks.push_back(std::move(registration));
    }

    /** A conversation whose assistant entry calls `name` with `arguments`; returns the tool task id. */
    std::int64_t callTool(const std::string& name, const Json& arguments, std::int64_t* conversationOut = nullptr) {
        m_conversation = m_fixture.newConversation();
        Json call = Json::object({{"type", "toolCall"}, {"id", "call-1"}, {"name", name}, {"arguments", arguments}});
        Json assistant = Json::object({{"role", "assistant"}, {"content", Json::array({call})}, {"stopReason", "toolUse"}, {"timestamp", 1}});
        m_assistant = m_fixture.append(m_conversation, Json::object({{"kind", "pi.assistant"}, {"model", Json::array({assistant})}}));
        if (conversationOut != nullptr) {
            *conversationOut = m_conversation;
        }
        return m_fixture.createTask(m_conversation, "pi.tool", Json::object({{"assistant", m_assistant}, {"callId", "call-1"}}));
    }

    void rewindToExecute(std::int64_t taskId, const std::string& replay) {
        ASSERT_TRUE(m_fixture.session().commit([&](Transaction& tx) -> Result<void> {
            auto record = tx.task(taskId);
            if (!record || !*record) {
                return std::unexpected(Error{"x", "missing"});
            }
            Json state = Json::object({{"status", "running"},
                                       {"checkpoint", Json::object({{"phase", "execute"}, {"arguments", Json::object({{"text", "x"}})}, {"replay", replay}})}});
            return tx.setTask(TaskRecords().withState(**record, state));
        }).has_value());
    }

    /** Runs the task to its end and returns the result entry. */
    Json finish(std::int64_t taskId) {
        m_fixture.resume();
        EXPECT_TRUE(m_fixture.eventually([&] { return m_fixture.terminal(taskId); })) << m_fixture.status(taskId);
        auto record = m_fixture.task(taskId);
        const Json& result = record->at("state").at("outcome").contains("result") ? record->at("state").at("outcome").at("result") : Json::object();
        if (!result.contains("entryId")) {
            return Json(nullptr);
        }
        for (const Json& entry : m_fixture.entries(m_conversation)) {
            if (entry.at("id") == result.at("entryId")) {
                return entry;
            }
        }
        return Json(nullptr);
    }

    std::string resultText(const Json& entry) {
        std::string text;
        for (const Json& block : entry.at("model")[0].at("content")) {
            text += block.at("text").get<std::string>() + "|";
        }
        return text;
    }

    TaskFixture m_fixture;
    std::mutex m_mutex;
    std::vector<ToolRegistration> m_tools;
    std::vector<HookRegistration> m_hooks;
    std::int64_t m_conversation = 0;
    std::int64_t m_assistant = 0;

    void SetUp() override {
        m_fixture.open();
    }
};

TEST_F(ToolTaskTest, ExecutesTheToolAndAppendsItsResult) {
    addTool(tool("echo", [this](const Json& args, IToolExecutionApi&) -> Result<ToolExecutionResult> {
        return textResult("echo " + args.at("text").get<std::string>());
    }));
    const std::int64_t id = callTool("echo", Json::object({{"text", "hi"}}));
    Json entry = finish(id);
    EXPECT_EQ(m_fixture.outcome(id), "completed");
    EXPECT_EQ(entry.at("kind"), "pi.tool-result");
    EXPECT_EQ(resultText(entry), "echo hi|");
    EXPECT_EQ(entry.at("model")[0].at("toolCallId"), "call-1");
    EXPECT_EQ(entry.at("model")[0].at("isError"), false);
    EXPECT_EQ(m_fixture.task(id)->at("state").at("outcome").at("result").at("entryId"), entry.at("id"));
}

TEST_F(ToolTaskTest, ArgumentsAreCoercedAgainstTheSchemaBeforeExecution) {
    std::mutex mutex;
    Json seen;
    addTool(tool("count", [&](const Json& args, IToolExecutionApi&) -> Result<ToolExecutionResult> {
        const std::lock_guard<std::mutex> lock(mutex);
        seen = args;
        return textResult("ok");
    }));
    finish(callTool("count", Json::object({{"text", "x"}, {"count", "3"}})));
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(seen.at("count"), 3);
}

TEST_F(ToolTaskTest, UnavailableToolAndInvalidArgumentsGiveHarnessErrorsAndStillComplete) {
    addTool(tool("echo", [this](const Json&, IToolExecutionApi&) -> Result<ToolExecutionResult> { return textResult("never"); }));
    const std::int64_t missing = callTool("nope", Json::object());
    Json unavailable = finish(missing);
    EXPECT_EQ(m_fixture.outcome(missing), "completed");
    EXPECT_EQ(unavailable.at("model")[0].at("isError"), true);
    EXPECT_NE(resultText(unavailable).find("[error] Tool nope is not available"), std::string::npos);
    EXPECT_EQ(unavailable.at("data").at("diagnostics")[0].at("code"), "tool_unavailable");
    const std::int64_t invalid = callTool("echo", Json::object({{"count", 1}}));
    Json rejected = finish(invalid);
    EXPECT_EQ(rejected.at("data").at("diagnostics")[0].at("code"), "invalid_arguments");
    EXPECT_EQ(m_fixture.outcome(invalid), "completed");
}

TEST_F(ToolTaskTest, PrepareArgumentsRepairsBeforeValidation) {
    ToolRegistration repaired = tool("echo", [this](const Json& args, IToolExecutionApi&) -> Result<ToolExecutionResult> {
        return textResult(args.at("text").get<std::string>());
    });
    repaired.prepareArguments = [](const Json& args) -> Result<Json> {
        Json fixed = args;
        if (fixed.contains("txt")) {
            fixed["text"] = fixed.at("txt");
            fixed.erase("txt");
        }
        return fixed;
    };
    addTool(repaired);
    EXPECT_EQ(resultText(finish(callTool("echo", Json::object({{"txt", "fixed"}})))), "fixed|");
    ToolRegistration broken = tool("broken", [this](const Json&, IToolExecutionApi&) -> Result<ToolExecutionResult> { return textResult("never"); });
    broken.prepareArguments = [](const Json&) -> Result<Json> { return std::unexpected(Error{"x", "cannot repair"}); };
    addTool(broken);
    Json entry = finish(callTool("broken", Json::object()));
    EXPECT_EQ(entry.at("data").at("diagnostics")[0].at("code"), "invalid_arguments");
    EXPECT_NE(resultText(entry).find("cannot repair"), std::string::npos);
}

TEST_F(ToolTaskTest, BeforeToolHooksBlockOrReplaceArguments) {
    std::atomic<int> executions{0};
    std::mutex mutex;
    Json seen;
    addTool(tool("echo", [&](const Json& args, IToolExecutionApi&) -> Result<ToolExecutionResult> {
        ++executions;
        const std::lock_guard<std::mutex> lock(mutex);
        seen = args;
        return textResult("ran");
    }));
    addHook("beforeTool", [](const Json& call, IHookApi&) -> Result<std::optional<Json>> {
        if (call.at("arguments").at("text") == "bad") {
            return std::optional<Json>(Json::object({{"block", "not allowed"}}));
        }
        if (call.at("arguments").at("text") == "swap") {
            return std::optional<Json>(Json::object({{"arguments", Json::object({{"text", "swapped"}})}}));
        }
        return std::optional<Json>();
    });
    Json blocked = finish(callTool("echo", Json::object({{"text", "bad"}})));
    EXPECT_EQ(executions.load(), 0);
    EXPECT_EQ(blocked.at("data").at("diagnostics")[0].at("code"), "blocked");
    EXPECT_NE(resultText(blocked).find("Tool call blocked: not allowed"), std::string::npos);
    finish(callTool("echo", Json::object({{"text", "swap"}})));
    EXPECT_EQ(executions.load(), 1);
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(seen.at("text"), "swapped");
}

TEST_F(ToolTaskTest, AThrowingBeforeHookBlocksTheCall) {
    addTool(tool("echo", [this](const Json&, IToolExecutionApi&) -> Result<ToolExecutionResult> { return textResult("never"); }));
    addHook("beforeTool", [](const Json&, IHookApi&) -> Result<std::optional<Json>> { return std::unexpected(Error{"x", "hook crashed"}); });
    Json entry = finish(callTool("echo", Json::object({{"text", "a"}})));
    EXPECT_NE(resultText(entry).find("Tool call blocked: hook crashed"), std::string::npos);
}

TEST_F(ToolTaskTest, AfterToolReplacesTheResult) {
    addTool(tool("echo", [this](const Json&, IToolExecutionApi&) -> Result<ToolExecutionResult> { return textResult("original"); }));
    addHook("afterTool", [](const Json& payload, IHookApi&) -> Result<std::optional<Json>> {
        Json replaced = payload.at("result");
        replaced["content"] = Json::array({Json::object({{"type", "text"}, {"text", "replaced"}})});
        return std::optional<Json>(replaced);
    });
    EXPECT_EQ(resultText(finish(callTool("echo", Json::object({{"text", "a"}})))), "replaced|");
}

TEST_F(ToolTaskTest, RunningOutputBecomesTheContentAndDetailsAndControlAreKept) {
    addTool(tool("stream", [](const Json&, IToolExecutionApi& api) -> Result<ToolExecutionResult> {
        (void)api.output("part one\n");
        (void)api.output("part two\n");
        (void)api.details(Json::object({{"lines", 2}}));
        ToolExecutionResult result;
        result.control = Json::object({{"terminate", true}});
        return result;
    }));
    const std::int64_t id = callTool("stream", Json::object({{"text", "x"}}));
    Json entry = finish(id);
    EXPECT_EQ(resultText(entry), "part one\npart two\n|");
    EXPECT_EQ(entry.at("model")[0].at("details").at("lines"), 2);
    EXPECT_EQ(m_fixture.task(id)->at("state").at("outcome").at("result").at("control").at("terminate"), true);
}

TEST_F(ToolTaskTest, ExplicitContentIsBoundedWithATruncationDiagnostic) {
    ToolRegistration limited = tool("big", [this](const Json&, IToolExecutionApi&) -> Result<ToolExecutionResult> {
        return textResult("l1\nl2\nl3\nl4\n");
    });
    OutputLimits limits;
    limits.maxLines = 2;
    limits.retain = "head";
    limited.outputLimits = limits;
    addTool(limited);
    Json entry = finish(callTool("big", Json::object({{"text", "x"}})));
    const Json& content = entry.at("model")[0].at("content");
    ASSERT_EQ(content.size(), 2u);
    EXPECT_EQ(content[0].at("text"), "l1\nl2\n");
    EXPECT_NE(content[1].at("text").get<std::string>().find("Output truncated to its beginning: 2 lines, 6 bytes dropped"), std::string::npos);
    EXPECT_EQ(entry.at("data").at("diagnostics")[0].at("code"), "truncated");
}

TEST_F(ToolTaskTest, AFailingToolStillWritesAnErrorResultAndEndsFailed) {
    addTool(tool("boom", [](const Json&, IToolExecutionApi& api) -> Result<ToolExecutionResult> {
        (void)api.output("before the failure\n");
        return std::unexpected(Error{"x", "it broke"});
    }));
    const std::int64_t id = callTool("boom", Json::object({{"text", "x"}}));
    Json entry = finish(id);
    EXPECT_EQ(m_fixture.outcome(id), "failed");
    EXPECT_EQ(m_fixture.task(id)->at("state").at("outcome").at("error").at("message"), "Tool boom threw");
    EXPECT_EQ(entry.at("model")[0].at("isError"), true);
    EXPECT_NE(resultText(entry).find("before the failure"), std::string::npos);
    EXPECT_NE(resultText(entry).find("[error] it broke"), std::string::npos);
    EXPECT_EQ(entry.at("data").at("diagnostics")[0].at("code"), "tool_error");
}

TEST_F(ToolTaskTest, ToolsCanCreateTasksAndCommitThroughTheirApi) {
    std::atomic<std::int64_t> created{0};
    auto child = std::make_shared<TaskDefinition>();
    child->name = "child";
    child->initial = [](const Json&) { return Json::object({{"phase", "start"}}); };
    m_fixture.install("tasks", {child});
    addTool(tool("spawn", [&](const Json&, IToolExecutionApi& api) -> Result<ToolExecutionResult> {
        TaskOptions options;
        options.ownership = Json::object({{"kind", "task"}, {"taskId", api.taskId()}});
        auto id = api.createTask("child", Json::object(), options);
        if (!id) {
            return std::unexpected(id.error());
        }
        created = *id;
        return textResult("spawned");
    }));
    const std::int64_t id = callTool("spawn", Json::object({{"text", "x"}}));
    m_fixture.resume();
    ASSERT_TRUE(m_fixture.eventually([&] { return created.load() != 0; }));
    auto record = m_fixture.task(created.load());
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->at("kind"), "child");
    EXPECT_EQ(record->at("owner"), id);
}

/** Rewinds a freshly created tool task to the `execute` phase, as if the process stopped after recording intent. */
TEST_F(ToolTaskTest, RecoveryReRunsASafeTool) {
    std::atomic<int> executions{0};
    ToolRegistration safe = tool("safe", [&](const Json&, IToolExecutionApi&) -> Result<ToolExecutionResult> {
        ++executions;
        return textResult("again");
    });
    safe.replay = "safe";
    addTool(safe);
    const std::int64_t taskId = callTool("safe", Json::object({{"text", "x"}}));
    rewindToExecute(taskId, "safe");
    Json rerun = finish(taskId);
    EXPECT_EQ(resultText(rerun), "again|");
    EXPECT_EQ(m_fixture.outcome(taskId), "completed");
    EXPECT_EQ(executions.load(), 1);
}

TEST_F(ToolTaskTest, RecoveryInterruptsAnUnsafeTool) {
    std::atomic<int> executions{0};
    addTool(tool("unsafe", [&](const Json&, IToolExecutionApi&) -> Result<ToolExecutionResult> {
        ++executions;
        return textResult("never");
    }));
    const std::int64_t taskId = callTool("unsafe", Json::object({{"text", "x"}}));
    rewindToExecute(taskId, "unsafe");
    Json interrupted = finish(taskId);
    EXPECT_EQ(m_fixture.outcome(taskId), "failed");
    EXPECT_EQ(executions.load(), 0);
    EXPECT_NE(resultText(interrupted).find("was interrupted and may have partially run"), std::string::npos);
    EXPECT_EQ(interrupted.at("data").at("diagnostics")[0].at("code"), "interrupted");
}

TEST_F(ToolTaskTest, RecoveryDoesNotRerunWhenOnlyTheStoredPolicyIsSafe) {
    std::atomic<int> executions{0};
    // The tool's current policy is unsafe even though the stored intent said safe.
    addTool(tool("changed", [&](const Json&, IToolExecutionApi&) -> Result<ToolExecutionResult> {
        ++executions;
        return textResult("never");
    }));
    const std::int64_t taskId = callTool("changed", Json::object({{"text", "x"}}));
    rewindToExecute(taskId, "safe");
    finish(taskId);
    EXPECT_EQ(m_fixture.outcome(taskId), "failed");
    EXPECT_EQ(executions.load(), 0);
}

TEST_F(ToolTaskTest, AbortSettlesAnAbortedResultFromTheDurablePartialOutput) {
    WaitGate started;
    addTool(tool("slow", [&](const Json&, IToolExecutionApi& api) -> Result<ToolExecutionResult> {
        (void)api.output("partial\n");
        (void)api.details(Json::object({{"step", 1}}));
        started.open();
        (void)WaitGate().wait({&api.signal()});
        return std::unexpected(Error{"aborted", "aborted"});
    }));
    const std::int64_t id = callTool("slow", Json::object({{"text", "x"}}));
    // The slot a generation would have created for this call.
    ASSERT_TRUE(m_fixture.session().commit([&](Transaction& tx) -> Result<void> {
        DocAddressArgs args;
        args.owner = m_conversation;
        auto live = tx.doc(BuiltinDocuments().live(), args);
        if (!live) {
            return std::unexpected(live.error());
        }
        (**live)["tools"] = Json::array({Json::object({{"callId", "call-1"}, {"name", "slow"}, {"taskId", id}, {"status", "pending"}})});
        return {};
    }).has_value());
    m_fixture.resume();
    ASSERT_TRUE(started.wait({}).has_value());
    ASSERT_TRUE(m_fixture.scheduler().abort(id).has_value());
    ASSERT_TRUE(m_fixture.eventually([&] { return m_fixture.terminal(id); }));
    EXPECT_EQ(m_fixture.outcome(id), "aborted");
    Json entry;
    for (const Json& candidate : m_fixture.entries(m_conversation)) {
        if (candidate.at("kind") == "pi.tool-result") {
            entry = candidate;
        }
    }
    EXPECT_EQ(entry.at("model")[0].at("isError"), true);
    EXPECT_NE(resultText(entry).find("[error] Tool slow was aborted"), std::string::npos);
    EXPECT_EQ(entry.at("data").at("diagnostics").back().at("code"), "aborted");
    Json live = m_fixture.document(BuiltinDocuments().live(), m_conversation);
    EXPECT_EQ(live.at("tools")[0].at("status"), "done");
    EXPECT_EQ(live.at("tools")[0].at("entry"), entry.at("id"));
}
