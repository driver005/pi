export module pi.testing.durable_harness_fixture;

import std;
export import pi.ai.faux_provider;
export import pi.durable.memory_storage;
export import pi.support.harness;
export import pi.testing.fake_model_runtime;
export import pi.testing.fixed_clock;
export import pi.testing.inline_executor;

/**
 * A durable harness over memory storage for tests of the layers above it: the built-in tasks in a registry, a faux
 * provider behind a fake model runtime (model `faux/m`), and a root conversation configured with that model. A test
 * enqueues faux responses and talks to `root()`.
 */
export class DurableHarnessFixture {
public:
    DurableHarnessFixture()
        : m_registry(BuiltinTasks().all()),
          m_faux(m_executor, m_clock) {
        Model model;
        model.id = "m";
        model.name = "Faux model";
        model.provider = "faux";
        model.api = "faux";
        model.contextWindow = 100000;
        model.maxTokens = 4096;
        m_models.addModel(model);
        m_models.setAuthenticated("faux", true);
        m_models.setStreamHandler([this](const Model& model, const TranscriptContext& context, const StreamOptions& options) {
            return m_faux.stream(model, context, options);
        });
    }

    ~DurableHarnessFixture() {
        if (m_harness) {
            (void)m_harness->close();
        }
    }

    DurableHarnessFixture(const DurableHarnessFixture&) = delete;
    DurableHarnessFixture& operator=(const DurableHarnessFixture&) = delete;

    /** Opens the harness and creates the root conversation. */
    Result<void> open() {
        HarnessOptions options;
        options.models = &m_models;
        options.registry = &m_registry;
        options.now = [] { return std::int64_t(1000); };
        m_harness = std::make_unique<Harness>(std::make_shared<MemoryStorage>(), options);
        if (auto opened = m_harness->open(); !opened) {
            return opened;
        }
        ConversationCreateOptions create;
        create.agent = Json::object({{"model", Json::object({{"provider", "faux"}, {"modelId", "m"}})}});
        auto root = m_harness->root(create);
        if (!root) {
            return std::unexpected(root.error());
        }
        m_root = *root;
        return {};
    }

    Harness& harness() {
        return *m_harness;
    }

    std::shared_ptr<Conversation> root() {
        return m_root;
    }

    FauxProvider& faux() {
        return m_faux;
    }

    FakeModelRuntime& models() {
        return m_models;
    }

    Registry& registry() {
        return m_registry;
    }

    /** An input submission draft with `text` as its content. */
    SubmissionDraft input(const std::string& text) const {
        SubmissionDraft draft;
        draft.type = "input";
        draft.content = text;
        return draft;
    }

private:
    InlineExecutor m_executor;
    FixedClock m_clock;
    Registry m_registry;
    FauxProvider m_faux;
    FakeModelRuntime m_models;
    std::unique_ptr<Harness> m_harness;
    std::shared_ptr<Conversation> m_root;
};
