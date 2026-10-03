#include <gtest/gtest.h>

import std;
import pi.ai.faux_provider;
import pi.durable.jsonl_storage;
import pi.durable.memory_storage;
import pi.support.delta_applier;
import pi.support.harness;
import pi.support.json_equality;
import pi.testing.fake_file_system;
import pi.testing.fake_model_runtime;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;

/** Owns a MemoryStorage-backed JSONL store over a shared fake file system, so a "restart" reopens the same files. */
class JsonlBundle {
public:
    JsonlBundle(IFileSystem& files, const std::string& directory) : m_storage(m_memory, files, directory, JsonlStorageOptions{false}) {}

    Result<void> open() {
        return m_storage.open();
    }

    IStorage& storage() {
        return m_storage;
    }

private:
    MemoryStorage m_memory;
    JsonlStorage m_storage;
};

class HarnessTest : public ::testing::Test {
protected:
    HarnessTest() : m_registry(BuiltinTasks().all()), m_faux(m_executor, m_clock) {
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
        m_settings.retry = Json::object({{"baseDelayMs", 50}});
    }

    ~HarnessTest() override {
        if (m_harness) {
            (void)m_harness->close();
        }
    }

    HarnessOptions options() {
        HarnessOptions options;
        options.models = &m_models;
        options.registry = &m_registry;
        options.settings = [this] { return m_settings; };
        options.now = [this] { return m_time.load(); };
        options.onReport = [this](const Error& error) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_reports.push_back(error.message);
        };
        return options;
    }

    void openMemory() {
        m_harness = std::make_unique<Harness>(std::make_shared<MemoryStorage>(), options());
        ASSERT_TRUE(m_harness->open().has_value());
    }

    /** Opens a harness over the JSONL store in the shared fake file system, as a fresh process would. */
    void openJsonl() {
        m_bundle = std::make_shared<JsonlBundle>(m_files, "/data/session");
        ASSERT_TRUE(m_bundle->open().has_value());
        m_harness = std::make_unique<Harness>(std::shared_ptr<IStorage>(m_bundle, &m_bundle->storage()), options());
        ASSERT_TRUE(m_harness->open().has_value());
    }

    Json agentWithModel() {
        return Json::object({{"model", Json::object({{"provider", "faux"}, {"modelId", "m"}})}});
    }

    std::shared_ptr<Conversation> rootWithModel() {
        ConversationCreateOptions create;
        create.agent = agentWithModel();
        auto root = m_harness->root(create);
        EXPECT_TRUE(root.has_value()) << (root ? "" : root.error().message);
        return *root;
    }

    SubmissionDraft input(const std::string& text) {
        SubmissionDraft draft;
        draft.type = "input";
        draft.content = text;
        return draft;
    }

    template <typename Predicate>
    bool eventually(Predicate predicate) {
        for (int attempt = 0; attempt < 2000; ++attempt) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    InlineExecutor m_executor;
    Registry m_registry;
    FixedClock m_clock;
    std::atomic<std::int64_t> m_time{1000};
    FauxProvider m_faux;
    FakeModelRuntime m_models;
    FakeFileSystem m_files;
    std::shared_ptr<JsonlBundle> m_bundle;
    HarnessRunSettings m_settings;
    std::unique_ptr<Harness> m_harness;
    std::mutex m_mutex;
    std::vector<std::string> m_reports;
};

TEST_F(HarnessTest, RootIsCreatedOnceAndConversationsAreLookedUpById) {
    openMemory();
    auto root = m_harness->root();
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ((*root)->id(), 1);
    auto again = m_harness->root();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ((*again)->id(), 1);
    auto created = m_harness->createConversation();
    ASSERT_TRUE(created.has_value());
    EXPECT_NE((*created)->id(), 1);
    auto found = m_harness->conversation((*created)->id());
    ASSERT_TRUE(found.has_value() && *found);
    EXPECT_EQ((*found)->id(), (*created)->id());
    auto missing = m_harness->conversation(424242);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(*missing);
}

TEST_F(HarnessTest, EveryConversationStartsWithItsBuiltInDocuments) {
    openMemory();
    auto conversation = m_harness->createConversation();
    ASSERT_TRUE(conversation.has_value());
    BuiltinDocuments documents;
    DocAddressArgs args;
    args.owner = (*conversation)->id();
    for (const DocDefinition& definition : {documents.live(), documents.inbox(), documents.usage(), documents.agent()}) {
        auto snapshot = m_harness->session().snapshot(definition, args);
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_TRUE(snapshot->has_value()) << definition.kind;
    }
}

TEST_F(HarnessTest, CreationHookAndInitRunInTheCreatingCommit) {
    HarnessOptions custom = options();
    std::atomic<int> hooked{0};
    custom.conversationCreated = [&](Transaction&, const Json&) -> Result<void> {
        ++hooked;
        return {};
    };
    m_harness = std::make_unique<Harness>(std::make_shared<MemoryStorage>(), custom);
    ASSERT_TRUE(m_harness->open().has_value());
    ConversationCreateOptions create;
    std::int64_t initialized = 0;
    create.init = [&](Transaction&, std::int64_t id) -> Result<void> {
        initialized = id;
        return {};
    };
    create.agent = Json::object({{"instructions", "be brief"}});
    auto conversation = m_harness->createConversation(create);
    ASSERT_TRUE(conversation.has_value());
    EXPECT_EQ(hooked.load(), 1);
    EXPECT_EQ(initialized, (*conversation)->id());
    auto agent = (*conversation)->agent();
    ASSERT_TRUE(agent.has_value());
    EXPECT_EQ(*(*agent)->snapshot()->instructions, "be brief");
}

TEST_F(HarnessTest, SubmittingInputRunsAGenerationAndTheSubmissionSettles) {
    openMemory();
    m_faux.enqueue(m_faux.textResponse("hello there"));
    auto root = rootWithModel();
    auto submission = root->submit(input("hi"));
    ASSERT_TRUE(submission.has_value()) << submission.error().message;
    auto settled = (*submission)->wait();
    ASSERT_TRUE(settled.has_value()) << settled.error().message;
    EXPECT_EQ(settled->at("status"), "done");
    auto context = root->context();
    ASSERT_TRUE(context.has_value());
    // No extension supplies sections or tools, so no system entry: just the input and the answer.
    EXPECT_EQ(context->at("messages").size(), 2u);
    EXPECT_EQ(context->at("messages")[1].at("content")[0].at("text"), "hello there");
    ASSERT_TRUE(root->waitForIdle().has_value());
    auto usage = m_harness->usage();
    ASSERT_TRUE(usage.has_value());
    EXPECT_TRUE(usage->at("models").contains("faux/m"));
    auto page = root->entries(std::nullopt, std::nullopt, 10);
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->items.size(), 2u);
    EXPECT_EQ(page->items[0].at("kind"), "pi.assistant");
}

TEST_F(HarnessTest, ConfigureChangesTheAgentTheNextRunUses) {
    openMemory();
    auto root = rootWithModel();
    ASSERT_TRUE(root->configure(Json::object({{"thinkingLevel", "high"}, {"instructions", "Answer in French."}})).has_value());
    m_faux.enqueue(m_faux.textResponse("bonjour"));
    auto submission = root->submit(input("hi"));
    ASSERT_TRUE(submission.has_value());
    ASSERT_TRUE((*submission)->wait().has_value());
    auto context = root->context();
    ASSERT_TRUE(context.has_value());
    const Json& system = context->at("messages")[1];
    EXPECT_EQ(system.at("sections").at("instructions"), "<instructions>\nAnswer in French.\n</instructions>");
    auto agent = root->agent();
    EXPECT_EQ((*agent)->snapshot()->thinkingLevel, "high");
}

TEST_F(HarnessTest, ForkingCopiesTheAgentAsOfTheForkEntryAndRunsIndependently) {
    openMemory();
    auto root = rootWithModel();
    m_faux.enqueue(m_faux.textResponse("first"));
    auto submission = root->submit(input("hi"));
    ASSERT_TRUE(submission.has_value() && (*submission)->wait().has_value());
    ASSERT_TRUE(root->waitForIdle().has_value());
    auto page = root->entries(std::nullopt, std::nullopt, 10);
    const std::int64_t userEntry = page->items[1].at("id").get<std::int64_t>();
    // A later agent change must not leak into the fork made at an earlier entry.
    ASSERT_TRUE(root->configure(Json::object({{"cwd", "/later"}})).has_value());
    auto fork = root->fork(userEntry);
    ASSERT_TRUE(fork.has_value()) << fork.error().message;
    auto agent = (*fork)->agent();
    ASSERT_TRUE(agent.has_value());
    EXPECT_TRUE((*agent)->snapshot()->model.has_value());
    EXPECT_FALSE((*agent)->snapshot()->cwd.has_value());
    m_faux.enqueue(m_faux.textResponse("on the fork"));
    auto forkSubmission = (*fork)->submit(input("again"));
    ASSERT_TRUE(forkSubmission.has_value() && (*forkSubmission)->wait().has_value());
    auto forkContext = (*fork)->context();
    ASSERT_TRUE(forkContext.has_value());
    // The fork inherits the shared history up to its fork entry and then its own turns.
    EXPECT_EQ(forkContext->at("messages").back().at("content")[0].at("text"), "on the fork");
    auto rootContext = root->context();
    EXPECT_EQ(rootContext->at("messages").size(), 2u);
}

TEST_F(HarnessTest, ManualCompactionSummarizesOldHistory) {
    m_settings.compaction = Json::object({{"keepRecentTokens", 30}});
    openMemory();
    auto root = rootWithModel();
    for (int i = 0; i < 6; ++i) {
        const std::string text = std::string(100, static_cast<char>('a' + i));
        SubmissionDraft write;
        write.type = "write";
        write.entry = Json::object({{"kind", "note"},
                                    {"model", Json::array({i % 2 == 0 ? Json::object({{"role", "user"}, {"content", text}, {"timestamp", 1}})
                                                                      : Json::object({{"role", "assistant"}, {"content", Json::array({Json::object({{"type", "text"}, {"text", text}})})},
                                                                                      {"stopReason", "stop"}, {"timestamp", 2}})})}});
        ASSERT_TRUE(root->submit(write).has_value());
    }
    m_faux.enqueue(m_faux.textResponse("the summary"));
    auto task = root->compact(std::string("be thorough"));
    ASSERT_TRUE(task.has_value());
    auto settled = m_harness->waitForTask(*task);
    ASSERT_TRUE(settled.has_value()) << settled.error().message;
    EXPECT_EQ(settled->at("state").at("outcome").at("status"), "completed");
    auto context = root->context();
    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(context->at("head").at("data").at("reason"), "manual");
    EXPECT_EQ(context->at("messages").size(), 3u);
}

TEST_F(HarnessTest, ResetStartsANewContext) {
    openMemory();
    auto root = rootWithModel();
    m_faux.enqueue(m_faux.textResponse("first"));
    auto first = root->submit(input("hi"));
    ASSERT_TRUE(first.has_value() && (*first)->wait().has_value());
    ASSERT_TRUE(root->waitForIdle().has_value());
    ASSERT_TRUE(root->reset(std::string("summary of before")).has_value());
    auto context = root->context();
    ASSERT_TRUE(context.has_value());
    ASSERT_EQ(context->at("messages").size(), 1u);
    EXPECT_EQ(context->at("messages")[0].at("content"), "summary of before");
}

TEST_F(HarnessTest, InspectShowsBlockedTasksAndAbortTaskOrphansThem) {
    openMemory();
    auto root = m_harness->root();
    ASSERT_TRUE(root.has_value());
    std::int64_t ghost = 0;
    ASSERT_TRUE((*root)->commit([&](Transaction& tx) -> Result<void> {
        TaskOptions options;
        options.ownership = Json::object({{"kind", "conversation"}});
        auto created = tx.createTask("custom.ghost", 1, Json::object(), Json::object({{"phase", "start"}}), options);
        if (!created) {
            return std::unexpected(created.error());
        }
        ghost = *created;
        return {};
    }).has_value());
    EXPECT_EQ(m_harness->inspect()->at("scheduling"), "paused");
    ASSERT_TRUE(m_harness->resume().has_value());
    auto report = m_harness->inspect();
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->at("scheduling"), "running");
    ASSERT_EQ(report->at("tasks").size(), 1u);
    EXPECT_EQ(report->at("tasks")[0].at("state").at("kind"), "blocked");
    EXPECT_EQ(report->at("tasks")[0].at("state").at("reason"), "missing_task");
    auto aborted = m_harness->abortTask(ghost);
    ASSERT_TRUE(aborted.has_value());
    auto record = m_harness->waitForTask(ghost);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->at("state").at("outcome").at("status"), "orphaned");
    EXPECT_TRUE(m_harness->getTask(ghost)->has_value());
    EXPECT_TRUE(m_harness->inspect()->at("tasks").empty());
}

TEST_F(HarnessTest, AbortingASubmissionWithdrawsItWhileQueued) {
    openMemory();
    auto root = rootWithModel();
    WaitGate streaming;
    WaitGate release;
    m_models.setStreamHandler([&](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        streaming.open();
        (void)release.wait({options.signal.get()});
        return m_faux.stream(model, context, options);
    });
    m_faux.enqueue(m_faux.textResponse("late"));
    auto first = root->submit(input("busy"));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(streaming.wait({}).has_value());
    auto queued = root->submit(input("queued"));
    ASSERT_TRUE(queued.has_value());
    auto inspection = m_harness->inspect();
    EXPECT_EQ(inspection->at("submissions").size(), 2u);
    EXPECT_EQ(*(*first)->abort(), "already_placed");
    EXPECT_EQ(*m_harness->abortSubmission((*queued)->id(), root->id()), "aborted");
    EXPECT_EQ(*m_harness->abortSubmission((*queued)->id(), root->id()), "settled");
    EXPECT_EQ(*m_harness->abortSubmission((*queued)->id(), root->id() + 50), "not_found");
    SubmissionDraft rejecting = input("rejected");
    rejecting.whenBusy = "reject";
    auto busy = root->submit(rejecting);
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(busy.error().code, "conversation_busy");
    release.open();
    ASSERT_TRUE((*first)->wait().has_value());
}

TEST_F(HarnessTest, ConversationAbortCancelsTheRunAndSettlesInputsAborted) {
    openMemory();
    auto root = rootWithModel();
    WaitGate streaming;
    WaitGate release;
    m_models.setStreamHandler([&](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        streaming.open();
        (void)release.wait({options.signal.get()});
        return m_faux.stream(model, context, options);
    });
    m_faux.enqueue(m_faux.textResponse("late"));
    auto submission = root->submit(input("hi"));
    ASSERT_TRUE(submission.has_value());
    ASSERT_TRUE(streaming.wait({}).has_value());
    // Abort signals the in-flight request, so the blocked handler is released without `release`.
    ASSERT_TRUE(root->abort().has_value());
    release.open();
    auto settled = (*submission)->wait();
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->at("status"), "unanswered");
    EXPECT_EQ(settled->at("reason"), "aborted");
}

TEST_F(HarnessTest, ARunInterruptedByCloseResumesAfterReopeningTheStore) {
    openJsonl();
    WaitGate streaming;
    m_models.setStreamHandler([&](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        streaming.open();
        (void)WaitGate().wait({options.signal.get()});
        return m_faux.stream(model, context, options);
    });
    auto root = rootWithModel();
    auto submission = root->submit(input("hello"));
    ASSERT_TRUE(submission.has_value());
    const std::int64_t submissionId = (*submission)->id();
    ASSERT_TRUE(streaming.wait({}).has_value());
    // The process stops mid-request: close signals the handler, joins it and writes no outcome.
    ASSERT_TRUE(m_harness->close().has_value());
    m_harness.reset();
    m_bundle.reset();

    m_models.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
        return m_faux.stream(model, context, options);
    });
    m_faux.enqueue(m_faux.textResponse("welcome back"));
    openJsonl();
    auto reacquired = m_harness->submission(submissionId);
    ASSERT_TRUE(reacquired.has_value() && *reacquired);
    auto status = (*reacquired)->status();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->at("status"), "placed");
    ASSERT_TRUE(m_harness->resume().has_value());
    auto settled = (*reacquired)->wait();
    ASSERT_TRUE(settled.has_value()) << settled.error().message;
    EXPECT_EQ(settled->at("status"), "done");
    auto again = m_harness->root();
    ASSERT_TRUE(again.has_value());
    auto context = (*again)->context();
    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(context->at("messages").back().at("content")[0].at("text"), "welcome back");
}

TEST_F(HarnessTest, OpenRequiresTheBuiltInTasksAndClosedHarnessesRefuseWork) {
    Registry bare;
    HarnessOptions custom = options();
    custom.registry = &bare;
    Harness incomplete(std::make_shared<MemoryStorage>(), custom);
    auto opened = incomplete.open();
    ASSERT_FALSE(opened.has_value());
    EXPECT_NE(opened.error().message.find("Registry lacks built-in tasks"), std::string::npos);
    openMemory();
    auto root = m_harness->root();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(m_harness->close().has_value());
    EXPECT_FALSE(m_harness->createConversation().has_value());
    EXPECT_FALSE(m_harness->conversation(1).has_value());
    EXPECT_FALSE((*root)->submit(input("late")).has_value());
    EXPECT_FALSE(m_harness->resume().has_value());
}

/** Records every publication of an observed state so the stream can be replayed over its first snapshot. */
class StateRecorder {
public:
    explicit StateRecorder(const std::shared_ptr<IReplicatedState>& state) : m_state(state), m_initial(state->snapshot()) {
        m_subscription = state->subscribe([this](const Json& ops, std::int64_t sequence, const ServiceContext&) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_ops.push_back(ops);
            m_sequences.push_back(sequence);
        });
    }

    ~StateRecorder() {
        m_state->unsubscribe(m_subscription);
    }

    /** The first snapshot with every recorded publication applied. */
    Json replayed() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        Json value = m_initial.value;
        for (const Json& ops : m_ops) {
            auto applied = DeltaApplier().apply(value, ops);
            EXPECT_TRUE(applied.has_value());
            if (applied) {
                value = *applied;
            }
        }
        return value;
    }

    std::vector<std::int64_t> sequences() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_sequences;
    }

private:
    std::shared_ptr<IReplicatedState> m_state;
    ReplicatedStateSnapshot m_initial;
    std::uint64_t m_subscription = 0;
    std::mutex m_mutex;
    std::vector<Json> m_ops;
    std::vector<std::int64_t> m_sequences;
};

TEST_F(HarnessTest, ConversationViewFollowsCommittedEntriesAndDocuments) {
    openMemory();
    m_faux.enqueue(m_faux.textResponse("hello there"));
    auto root = rootWithModel();
    auto view = m_harness->viewState(root->id());
    ASSERT_TRUE(view.has_value()) << view.error().message;
    StateRecorder recorder(*view);
    const Json initial = (*view)->snapshot().value;
    EXPECT_EQ(initial.at("entries").size(), 0u);
    EXPECT_EQ(initial.at("conversation").at("id"), root->id());
    for (const char* kind : {"pi.agent", "pi.live", "pi.inbox", "pi.usage"}) {
        EXPECT_TRUE(initial.at("docs").contains(kind)) << kind;
    }
    auto submission = root->submit(input("hi"));
    ASSERT_TRUE(submission.has_value() && (*submission)->wait().has_value());
    ASSERT_TRUE(root->waitForIdle().has_value());
    ASSERT_TRUE(eventually([&] { return (*view)->snapshot().value.at("entries").size() == 2u; }));
    ASSERT_TRUE(eventually([&] { return (*view)->snapshot().value.at("docs").at("pi.usage").at("models").contains("faux/m"); }));
    // Sequences count from 1 without gaps, and replaying the stream rebuilds the observed value.
    const std::vector<std::int64_t> sequences = recorder.sequences();
    for (std::size_t index = 0; index < sequences.size(); ++index) {
        EXPECT_EQ(sequences[index], static_cast<std::int64_t>(index) + 1);
    }
    // The state is updated before its listeners run, so wait for the recorder to reach the observed sequence.
    ASSERT_TRUE(eventually([&] { return static_cast<std::int64_t>(recorder.sequences().size()) == (*view)->snapshot().sequence; }));
    EXPECT_TRUE(JsonEquality().equal(recorder.replayed(), (*view)->snapshot().value));
}

TEST_F(HarnessTest, ConversationViewBuiltLateEqualsTheOneMaintainedFromTheStart) {
    openMemory();
    m_faux.enqueue(m_faux.textResponse("one"));
    auto root = rootWithModel();
    auto early = m_harness->viewState(root->id());
    ASSERT_TRUE(early.has_value());
    auto first = root->submit(input("hi"));
    ASSERT_TRUE(first.has_value() && (*first)->wait().has_value());
    ASSERT_TRUE(root->waitForIdle().has_value());
    ASSERT_TRUE(root->reset(std::string("summary")).has_value());
    ASSERT_TRUE(eventually([&] {
        const Json entries = (*early)->snapshot().value.at("entries");
        return !entries.empty() && entries[0].contains("head");
    }));
    // A second observer of the same conversation shares the mount.
    auto shared = m_harness->viewState(root->id());
    ASSERT_TRUE(shared.has_value());
    EXPECT_EQ(shared->get(), early->get());
    // A fresh mount (the early one released) is derived from storage and matches the maintained value.
    const Json maintained = (*early)->snapshot().value;
    early->reset();
    shared->reset();
    auto fresh = m_harness->viewState(root->id());
    ASSERT_TRUE(fresh.has_value());
    EXPECT_TRUE(JsonEquality().equal(maintained, (*fresh)->snapshot().value));
    auto missing = m_harness->viewState(424242);
    EXPECT_FALSE(missing.has_value());
}

TEST_F(HarnessTest, TaskGraphShowsLiveTasksAndDropsTerminalOnes) {
    openMemory();
    m_faux.enqueue(m_faux.textResponse("hello"));
    auto root = rootWithModel();
    auto graph = m_harness->taskGraph();
    ASSERT_TRUE(graph.has_value()) << graph.error().message;
    StateRecorder recorder(*graph);
    EXPECT_EQ((*graph)->snapshot().value.at("tasks").size(), 0u);
    auto submission = root->submit(input("hi"));
    ASSERT_TRUE(submission.has_value() && (*submission)->wait().has_value());
    ASSERT_TRUE(root->waitForIdle().has_value());
    ASSERT_TRUE(eventually([&] { return (*graph)->snapshot().value.at("tasks").empty(); }));
    // The generation task appeared in the stream while it was live.
    EXPECT_FALSE(recorder.sequences().empty());
    ASSERT_TRUE(eventually([&] { return static_cast<std::int64_t>(recorder.sequences().size()) == (*graph)->snapshot().sequence; }));
    const Json value = (*graph)->snapshot().value;
    EXPECT_TRUE(JsonEquality().equal(recorder.replayed(), value));
    auto fresh = m_harness->taskGraph();
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ((*fresh)->snapshot().value.at("tasks").size(), 0u);
}

TEST_F(HarnessTest, ClosedHarnessesRefuseViews) {
    openMemory();
    auto root = m_harness->root();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(m_harness->close().has_value());
    EXPECT_FALSE(m_harness->viewState(1).has_value());
    EXPECT_FALSE(m_harness->taskGraph().has_value());
}
