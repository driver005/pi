#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.ai.faux_provider;
import pi.ai.memory_credential_store;
import pi.ai.memory_models_store;
import pi.ai.model_runtime;
import pi.ai.provider_registry;
import pi.testing.fake_environment;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.inline_executor;
import pi.testing.scripted_process_runner;
import pi.testing.stub_oauth_flow;

/** A wire API without deferred support: only the defaults of IProvider apply. */
class PlainProvider : public IProvider {
public:
    std::string api() const override {
        return "plain";
    }

    std::shared_ptr<AssistantMessageStream> stream(const Model&, const TranscriptContext&, const StreamOptions&) override {
        return nullptr;
    }
};

class ModelRuntimeTest : public testing::Test {
protected:
    ModelRuntimeTest()
        : m_processes([](const ProcessRequest&) -> Result<ProcessResult> {
              ProcessResult result;
              result.exitCode = 0;
              result.output = "cmd-key\n";
              return result;
          }),
          m_configValues(m_environment, m_processes),
          m_credentials(m_configValues),
          m_envKeys(m_environment, m_files),
          m_faux(std::make_shared<FauxProvider>(m_executor, m_clock, "faux")),
          m_runtime(ModelRuntimeConfig{"/cfg/models.json", "/cfg/catalog"}, m_credentials, m_models,
                    m_files, m_providers, m_envKeys, m_configValues, m_clock, {}) {
        m_providers.registerProvider(m_faux);
        m_files.createDirectories("/cfg/catalog");
        m_files.writeFile("/cfg/catalog/models.json", R"({
            "openai":{"gpt-a":{"id":"gpt-a","name":"GPT A","api":"faux","baseUrl":"https://api.openai.com/v1","provider":"openai","contextWindow":1000,"maxTokens":100,"headers":{"x-model":"m"}}},
            "groq":{"llama":{"id":"llama","name":"Llama","api":"faux","baseUrl":"https://api.groq.com/openai/v1","provider":"groq"}}})");
    }

    void writeModelsJson(const std::string& text) {
        m_files.writeFile("/cfg/models.json", text);
    }

    StreamOptions captureOptions(StreamOptions& seen, Model& seenModel) {
        m_faux->enqueue([&](const TranscriptContext&, const StreamOptions& options, const Model& model) {
            seen = options;
            seenModel = model;
            return m_faux->textResponse("ok");
        });
        return StreamOptions{};
    }

    AssistantMessage run(const Model& model, const StreamOptions& options) {
        auto stream = m_runtime.stream(model, TranscriptContext{}, options);
        while (stream->next()) {
        }
        return *stream->result();
    }

    FakeEnvironment m_environment;
    FakeFileSystem m_files;
    FixedClock m_clock;
    InlineExecutor m_executor;
    ScriptedProcessRunner m_processes;
    ConfigValueResolver m_configValues;
    MemoryCredentialStore m_credentials;
    MemoryModelsStore m_models;
    ProviderRegistry m_providers;
    EnvKeyTable m_envKeys;
    std::shared_ptr<FauxProvider> m_faux;
    ModelRuntime m_runtime;
};

TEST_F(ModelRuntimeTest, TransformHeadersRunsLastOverTheAssembledHeaders) {
    m_environment.set("OPENAI_API_KEY", "sk");
    ASSERT_TRUE(m_runtime.reload().has_value());
    StreamOptions seen;
    Model seenModel;
    StreamOptions options = captureOptions(seen, seenModel);
    options.headers = {{"x-caller", "c"}};
    std::string transformedFor;
    options.transformHeaders = [&](const Model& model, const std::vector<std::pair<std::string, std::optional<std::string>>>& headers) {
        transformedFor = model.id;
        auto out = headers;
        out.erase(std::remove_if(out.begin(), out.end(), [](const auto& header) { return header.first == "x-model"; }), out.end());
        out.emplace_back("x-added", "yes");
        return out;
    };
    run(*m_runtime.find("openai", "gpt-a"), options);
    EXPECT_EQ(transformedFor, "gpt-a");
    EXPECT_FALSE(seen.transformHeaders);
    std::map<std::string, std::string> headers;
    for (const auto& [name, value] : seen.headers) {
        if (value) {
            headers[name] = *value;
        }
    }
    EXPECT_EQ(headers.count("x-model"), 0U);
    EXPECT_EQ(headers["x-caller"], "c");
    EXPECT_EQ(headers["x-added"], "yes");
}

TEST_F(ModelRuntimeTest, RequestsCarryTheConfiguredUserAgentUnlessAHeaderSetsOne) {
    m_environment.set("OPENAI_API_KEY", "sk");
    ASSERT_TRUE(m_runtime.reload().has_value());
    StreamOptions seen;
    Model seenModel;
    StreamOptions options = captureOptions(seen, seenModel);
    run(*m_runtime.find("openai", "gpt-a"), options);
    std::map<std::string, std::string> headers;
    for (const auto& [name, value] : seen.headers) {
        headers[name] = value.value_or("");
    }
    EXPECT_EQ(headers["User-Agent"], "pi");
    StreamOptions again = captureOptions(seen, seenModel);
    again.headers = {{"User-Agent", "mine"}};
    run(*m_runtime.find("openai", "gpt-a"), again);
    std::vector<std::string> agents;
    for (const auto& [name, value] : seen.headers) {
        if (name == "User-Agent" && value) {
            agents.push_back(*value);
        }
    }
    ASSERT_FALSE(agents.empty());
    EXPECT_EQ(agents.back(), "mine");
}

TEST_F(ModelRuntimeTest, LoadsCatalogModels) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    EXPECT_FALSE(m_runtime.error().has_value());
    EXPECT_EQ(m_runtime.find("openai", "gpt-a")->name, "GPT A");
    EXPECT_FALSE(m_runtime.find("openai", "missing").has_value());
    EXPECT_EQ(m_runtime.providerName("openai"), "OpenAI");
    EXPECT_EQ(m_runtime.models().size(), 2U);
}

TEST_F(ModelRuntimeTest, ModelsJsonAddsCustomProviderAndOverrides) {
    writeModelsJson(R"({"providers":{
        "ollama":{"baseUrl":"http://localhost:11434/v1","api":"faux","apiKey":"ollama",
                  "models":[{"id":"llama3","contextWindow":32000}]},
        "openai":{"modelOverrides":{"gpt-a":{"name":"Renamed"}}}}})");
    ASSERT_TRUE(m_runtime.reload().has_value());
    EXPECT_FALSE(m_runtime.error().has_value()) << m_runtime.error().value_or("");
    EXPECT_EQ(m_runtime.find("ollama", "llama3")->contextWindow, 32000);
    EXPECT_EQ(m_runtime.find("openai", "gpt-a")->name, "Renamed");
    EXPECT_TRUE(m_runtime.hasConfiguredAuth("ollama"));
}

TEST_F(ModelRuntimeTest, BadModelsJsonIsReportedAndIgnored) {
    writeModelsJson("{nope");
    ASSERT_TRUE(m_runtime.reload().has_value());
    ASSERT_TRUE(m_runtime.error().has_value());
    EXPECT_NE(m_runtime.error()->find("Failed to parse models.json"), std::string::npos);
    EXPECT_EQ(m_runtime.models().size(), 2U);
}

TEST_F(ModelRuntimeTest, ProviderConfigErrorKeepsBuiltinModels) {
    writeModelsJson(R"({"providers":{"groq":{"models":[{"id":"x","contextWindow":0}]}}})");
    ASSERT_TRUE(m_runtime.reload().has_value());
    ASSERT_TRUE(m_runtime.error().has_value());
    EXPECT_TRUE(m_runtime.find("groq", "llama").has_value());
}

TEST_F(ModelRuntimeTest, AvailableModelsFollowCredentials) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    EXPECT_TRUE(m_runtime.availableModels().empty());
    m_environment.set("GROQ_API_KEY", "gsk");
    const auto available = m_runtime.availableModels();
    ASSERT_EQ(available.size(), 1U);
    EXPECT_EQ(available[0].provider, "groq");
    const auto status = m_runtime.authStatus("groq");
    EXPECT_EQ(status.source, "environment");
    EXPECT_EQ(status.label, "GROQ_API_KEY");
}

TEST_F(ModelRuntimeTest, AuthStatusSources) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    EXPECT_FALSE(m_runtime.authStatus("openai").configured);
    m_runtime.setRuntimeApiKey("openai", "sk-run");
    EXPECT_EQ(m_runtime.authStatus("openai").source, "runtime");
    m_runtime.removeRuntimeApiKey("openai");
    Credential stored;
    stored.key = "sk-stored";
    m_credentials.modify("openai", [&](const auto&) { return Result<std::optional<Credential>>(std::optional<Credential>(stored)); });
    EXPECT_EQ(m_runtime.authStatus("openai").source, "stored");
}

TEST_F(ModelRuntimeTest, StreamInjectsAuthHeadersAndEnv) {
    m_environment.set("OPENAI_API_KEY", "sk-env");
    ASSERT_TRUE(m_runtime.reload().has_value());
    StreamOptions seen;
    Model seenModel;
    StreamOptions options = captureOptions(seen, seenModel);
    options.headers = {{"x-caller", "c"}};
    const AssistantMessage message = run(*m_runtime.find("openai", "gpt-a"), options);
    EXPECT_EQ(message.stopReason, StopReason::Stop);
    EXPECT_EQ(seen.apiKey, "sk-env");
    HeaderMerger headers;
    HttpHeaders list;
    for (const auto& entry : seen.headers) {
        if (entry.second) {
            list.emplace_back(entry.first, *entry.second);
        }
    }
    EXPECT_EQ(headers.find(list, "x-model"), "m");
    EXPECT_EQ(headers.find(list, "x-caller"), "c");
}

TEST_F(ModelRuntimeTest, CallerApiKeyWinsOverResolvedKey) {
    m_environment.set("OPENAI_API_KEY", "sk-env");
    ASSERT_TRUE(m_runtime.reload().has_value());
    StreamOptions seen;
    Model seenModel;
    StreamOptions options = captureOptions(seen, seenModel);
    options.apiKey = "sk-caller";
    run(*m_runtime.find("openai", "gpt-a"), options);
    EXPECT_EQ(seen.apiKey, "sk-caller");
}

TEST_F(ModelRuntimeTest, UnconfiguredProviderYieldsErrorStream) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    const AssistantMessage message = run(*m_runtime.find("openai", "gpt-a"), StreamOptions{});
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_EQ(message.errorMessage, "Provider is not configured: openai");
}

TEST_F(ModelRuntimeTest, UnregisteredApiYieldsErrorStream) {
    m_environment.set("OPENAI_API_KEY", "sk");
    ASSERT_TRUE(m_runtime.reload().has_value());
    Model model = *m_runtime.find("openai", "gpt-a");
    model.api = "missing-api";
    EXPECT_EQ(run(model, StreamOptions{}).errorMessage, "No API provider registered for api: missing-api");
}

TEST_F(ModelRuntimeTest, ModelsJsonHeadersAndPerModelHeadersResolved) {
    m_environment.set("TEAM", "core");
    writeModelsJson(R"({"providers":{"ollama":{"baseUrl":"http://x/v1","api":"faux","apiKey":"k",
        "headers":{"x-provider":"$TEAM"},
        "models":[{"id":"m1","headers":{"x-model-cfg":"${TEAM}-model"}}]}}})");
    ASSERT_TRUE(m_runtime.reload().has_value());
    StreamOptions seen;
    Model seenModel;
    StreamOptions options = captureOptions(seen, seenModel);
    run(*m_runtime.find("ollama", "m1"), options);
    HeaderMerger merger;
    HttpHeaders list;
    for (const auto& entry : seen.headers) {
        list.emplace_back(entry.first, entry.second.value_or(""));
    }
    EXPECT_EQ(merger.find(list, "x-provider"), "core");
    EXPECT_EQ(merger.find(list, "x-model-cfg"), "core-model");
    EXPECT_EQ(seen.apiKey, "k");
}

TEST_F(ModelRuntimeTest, RegisterProviderAddsAndRemovesModels) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    const Json config = Json::parse(R"({"name":"Plugin","baseUrl":"http://p/v1","api":"faux","apiKey":"k","models":[{"id":"pm"}]})");
    ASSERT_TRUE(m_runtime.registerProvider("plugin", config).has_value());
    EXPECT_TRUE(m_runtime.find("plugin", "pm").has_value());
    EXPECT_EQ(m_runtime.providerName("plugin"), "Plugin");
    m_runtime.unregisterProvider("plugin");
    EXPECT_FALSE(m_runtime.find("plugin", "pm").has_value());
}

TEST_F(ModelRuntimeTest, RegisterProviderRejectsInvalidConfig) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    const auto result = m_runtime.registerProvider("plugin", Json::parse(R"({"models":[{"id":"pm"}]})"));
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(m_runtime.find("plugin", "pm").has_value());
}

TEST_F(ModelRuntimeTest, RemoteStoreOverlayAddsModelsWhenNewerThanCatalog) {
    m_files.setNowMs(1000);
    m_files.writeFile("/cfg/catalog/models.json", m_files.content("/cfg/catalog/models.json"));
    ModelsStoreEntry entry;
    entry.models = Json::parse(R"([{"id":"gpt-new","api":"faux","baseUrl":"https://api.openai.com/v1","name":"New"}])");
    entry.lastModified = 5000;
    m_models.write("openai", entry);
    ASSERT_TRUE(m_runtime.reload().has_value());
    EXPECT_TRUE(m_runtime.find("openai", "gpt-new").has_value());
    EXPECT_TRUE(m_runtime.find("openai", "gpt-a").has_value());
    // An entry older than the catalog is ignored.
    entry.lastModified = 10;
    m_models.write("openai", entry);
    ASSERT_TRUE(m_runtime.reload().has_value());
    EXPECT_FALSE(m_runtime.find("openai", "gpt-new").has_value());
}

TEST_F(ModelRuntimeTest, RadiusCatalogAppliesWithoutALastModifiedAndReplacesTheBaselineOfOtherGateways) {
    m_files.writeFile("/cfg/catalog/models.json", R"({"radius":{"base":{"id":"base","name":"Base","api":"faux","baseUrl":"https://radius.pi.dev","provider":"radius"}}})");
    ModelsStoreEntry entry;
    entry.models = Json::parse(R"([{"id":"dyn","name":"Dyn","api":"faux","baseUrl":"https://gw/api"},{"id":"base","name":"Renamed","api":"faux","baseUrl":"https://gw/api"}])");
    entry.checkedAt = 1;
    ASSERT_TRUE(m_models.write("radius", entry).has_value());
    ASSERT_TRUE(m_runtime.reload().has_value());
    ASSERT_TRUE(m_runtime.find("radius", "dyn").has_value());
    EXPECT_EQ(m_runtime.find("radius", "base")->name, "Renamed");

    ModelRuntime custom(ModelRuntimeConfig{"/cfg/models.json", "/cfg/catalog", "pi", "radius.example.com"}, m_credentials, m_models, m_files, m_providers, m_envKeys, m_configValues, m_clock, {});
    ASSERT_TRUE(custom.reload().has_value());
    EXPECT_TRUE(custom.find("radius", "dyn").has_value());
    EXPECT_EQ(custom.find("radius", "base")->name, "Renamed") << "the stored catalog replaces the generated model of the same id";
    ASSERT_TRUE(m_models.remove("radius").has_value());
    ASSERT_TRUE(custom.reload().has_value());
    EXPECT_FALSE(custom.find("radius", "base").has_value()) << "the generated Radius models belong to the default gateway";
    ASSERT_TRUE(m_runtime.reload().has_value());
    EXPECT_TRUE(m_runtime.find("radius", "base").has_value());
}

TEST_F(ModelRuntimeTest, DeferredResponsesAreFetchedAndCancelledThroughTheProvider) {
    m_environment.set("OPENAI_API_KEY", "sk");
    ASSERT_TRUE(m_runtime.reload().has_value());
    const Model model = *m_runtime.find("openai", "gpt-a");
    m_faux->setDeferredBehavior(1, 5);
    m_faux->enqueue(m_faux->textResponse("final"));
    StreamOptions options;
    options.deferred = true;
    const AssistantMessage deferred = run(model, options);
    ASSERT_EQ(deferred.stopReason, StopReason::Deferred);
    ASSERT_TRUE(deferred.deferred.has_value());
    EXPECT_EQ(deferred.deferred->pollAfterMs, 5);

    auto fetch = [&] {
        auto stream = m_runtime.fetchDeferred(model, *deferred.deferred, StreamOptions{});
        while (stream->next()) {
        }
        return *stream->result();
    };
    EXPECT_EQ(fetch().stopReason, StopReason::Deferred);
    const AssistantMessage answered = fetch();
    EXPECT_EQ(answered.stopReason, StopReason::Stop);
    EXPECT_EQ(std::get<TextContent>(answered.content[0]).text, "final");
    EXPECT_EQ(m_faux->deferredFetchCount(), 2);

    ASSERT_TRUE(m_runtime.cancelDeferred(model, *deferred.deferred, StreamOptions{}).has_value());
    ASSERT_EQ(m_faux->cancelledDeferred().size(), 1u);
    EXPECT_EQ(m_faux->cancelledDeferred()[0].id, deferred.deferred->id);
}

TEST_F(ModelRuntimeTest, DeferredRequestsNeedCredentials) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    const Model model = *m_runtime.find("openai", "gpt-a");
    DeferredHandle handle;
    handle.id = "h";
    auto stream = m_runtime.fetchDeferred(model, handle, StreamOptions{});
    while (stream->next()) {
    }
    EXPECT_EQ(stream->result()->errorMessage, "Provider is not configured: openai");
    auto cancelled = m_runtime.cancelDeferred(model, handle, StreamOptions{});
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().message, "Provider is not configured: openai");
}

TEST_F(ModelRuntimeTest, ProvidersWithoutDeferredSupportRefuseCleanly) {
    m_environment.set("OPENAI_API_KEY", "sk");
    ASSERT_TRUE(m_runtime.reload().has_value());
    m_providers.registerProvider(std::make_shared<PlainProvider>());
    Model model = *m_runtime.find("openai", "gpt-a");
    model.api = "plain";
    DeferredHandle handle;
    handle.id = "h";
    auto stream = m_runtime.fetchDeferred(model, handle, StreamOptions{});
    while (stream->next()) {
    }
    EXPECT_EQ(stream->result()->errorMessage, "Provider openai does not support deferred responses");
    auto cancelled = m_runtime.cancelDeferred(model, handle, StreamOptions{});
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().message, "Provider openai does not support deferred responses");
}

TEST_F(ModelRuntimeTest, PluginApisAreAddedOnceAndOnlyTheirsAreRemoved) {
    EXPECT_TRUE(m_runtime.registerApi(std::make_shared<PlainProvider>()).has_value());
    EXPECT_NE(m_providers.find("plain"), nullptr);
    const auto again = m_runtime.registerApi(std::make_shared<PlainProvider>());
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().message, "The API \"plain\" is already implemented");
    EXPECT_FALSE(m_runtime.registerApi(m_faux).has_value()) << "a built-in API cannot be replaced";
    m_runtime.unregisterApi("faux");
    EXPECT_NE(m_providers.find("faux"), nullptr) << "built-in APIs stay";
    m_runtime.unregisterApi("plain");
    EXPECT_EQ(m_providers.find("plain"), nullptr);
    EXPECT_TRUE(m_runtime.registerApi(std::make_shared<PlainProvider>()).has_value());
}

class VirtualModelRuntimeTest : public ModelRuntimeTest {
protected:
    VirtualModelDefinition definition(const std::string& provider, const std::string& id, const std::string& target) {
        VirtualModelDefinition out;
        out.provider = provider;
        out.id = id;
        out.name = "Auto";
        out.thinkingLevels = {ThinkingLevel::Low, ThinkingLevel::High};
        out.route = [this, target](const VirtualRouteRequest& request) -> Result<VirtualRoute> {
            m_seen = request;
            VirtualRoute route;
            route.model.provider = "openai";
            route.model.id = target;
            route.thinkingLevel = ThinkingLevel::High;
            return route;
        };
        return out;
    }

    VirtualRouteRequest m_seen;
};

TEST_F(VirtualModelRuntimeTest, VirtualModelsAreListedNextToThePhysicalOnesOfTheirProvider) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    ASSERT_TRUE(m_runtime.registerVirtualModel(definition("openai", "auto", "gpt-a")).has_value());
    const auto found = m_runtime.find("openai", "auto");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->api, "pi-virtual");
    EXPECT_FALSE(m_runtime.physicalModel("openai", "auto").has_value());
    EXPECT_TRUE(m_runtime.physicalModel("openai", "gpt-a").has_value());
    EXPECT_EQ(m_runtime.models().size(), 3U);
    // Availability follows the provider's credentials.
    EXPECT_FALSE(m_runtime.hasConfiguredAuth("openai"));
    for (const Model& model : m_runtime.availableModels()) {
        EXPECT_NE(model.provider, "openai");
    }
    m_runtime.setRuntimeApiKey("openai", "sk-test");
    EXPECT_EQ(m_runtime.availableModels().size(), 2U);
    m_runtime.unregisterVirtualModel("openai", "auto");
    EXPECT_FALSE(m_runtime.find("openai", "auto").has_value());
    EXPECT_EQ(m_runtime.models().size(), 2U);
}

TEST_F(VirtualModelRuntimeTest, AProviderOfOnlyVirtualModelsNeedsNoCredentials) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    ASSERT_TRUE(m_runtime.registerVirtualModel(definition("router", "auto", "gpt-a")).has_value());
    EXPECT_TRUE(m_runtime.hasConfiguredAuth("router"));
    EXPECT_EQ(m_runtime.authStatus("router").source, "virtual");
    const auto ids = m_runtime.providerIds();
    EXPECT_NE(std::ranges::find(ids, "router"), ids.end());
    const auto available = m_runtime.availableModels();
    EXPECT_EQ(std::ranges::count_if(available, [](const Model& model) { return model.provider == "router"; }), 1);
    m_runtime.unregisterVirtualModel("router", "auto");
    EXPECT_FALSE(m_runtime.hasConfiguredAuth("router"));
}

TEST_F(VirtualModelRuntimeTest, ARegistrationCannotShadowAPhysicalModelIdButHidesALaterOne) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    const auto rejected = m_runtime.registerVirtualModel(definition("openai", "gpt-a", "gpt-a"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message.find("conflicts with a physical model"), std::string::npos);
}

TEST_F(VirtualModelRuntimeTest, RequestsAreRoutedToACatalogModelWithCredentials) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    ASSERT_TRUE(m_runtime.registerVirtualModel(definition("router", "auto", "gpt-a")).has_value());
    VirtualResolveRequest request;
    request.model = *m_runtime.find("router", "auto");
    request.thinkingLevel = ThinkingLevel::Low;
    request.reason = "user";
    auto route = m_runtime.resolveVirtual(request);
    ASSERT_FALSE(route.has_value());
    EXPECT_NE(route.error().message.find("has no credentials"), std::string::npos);
    m_runtime.setRuntimeApiKey("openai", "sk-test");
    route = m_runtime.resolveVirtual(request);
    ASSERT_TRUE(route.has_value()) << route.error().message;
    EXPECT_EQ(route->model.id, "gpt-a");
    EXPECT_EQ(route->thinkingLevel, ThinkingLevel::Off);
    EXPECT_EQ(m_seen.thinkingLevel, ThinkingLevel::Low);
    EXPECT_EQ(m_seen.reason, "user");
}

TEST_F(VirtualModelRuntimeTest, ARouterCannotRouteToAVirtualModelAndUnroutedStreamsFail) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    ASSERT_TRUE(m_runtime.registerVirtualModel(definition("router", "first", "second")).has_value());
    ASSERT_TRUE(m_runtime.registerVirtualModel(definition("openai", "second", "second")).has_value());
    m_runtime.setRuntimeApiKey("openai", "sk-test");
    VirtualResolveRequest request;
    request.model = *m_runtime.find("router", "first");
    const auto route = m_runtime.resolveVirtual(request);
    ASSERT_FALSE(route.has_value());
    EXPECT_NE(route.error().message.find("not a physical model"), std::string::npos);

    const AssistantMessage message = run(*m_runtime.find("router", "first"), StreamOptions{});
    EXPECT_EQ(message.stopReason, StopReason::Error);
    EXPECT_NE(message.errorMessage->find("must be routed before streaming"), std::string::npos);
}

TEST_F(ModelRuntimeTest, APluginsOauthFlowRefreshesItsProvidersCredentialAndEndsOnUnregister) {
    ASSERT_TRUE(m_runtime.reload().has_value());
    ASSERT_TRUE(m_runtime.registerProvider("acme", Json::parse(R"({"baseUrl":"http://acme/v1","api":"faux","models":[{"id":"am"}]})")).has_value());
    Credential stored;
    stored.type = CredentialType::OAuth;
    stored.access = "old";
    stored.refresh = "r";
    stored.expires = 1;
    ASSERT_TRUE(m_credentials.modify("acme", [&](const std::optional<Credential>&) -> Result<std::optional<Credential>> { return std::optional<Credential>(stored); }).has_value());

    const auto without = m_runtime.getAuth("acme", std::nullopt, {});
    ASSERT_TRUE(without.has_value());
    EXPECT_FALSE(without->has_value()) << "a stored OAuth credential is useless without a flow";

    StubOauthFlow flow("acme");
    ASSERT_TRUE(m_runtime.registerOauthFlow("acme", flow).has_value());
    const auto refreshed = m_runtime.getAuth("acme", std::nullopt, {});
    ASSERT_TRUE(refreshed.has_value());
    ASSERT_TRUE(refreshed->has_value());
    EXPECT_EQ((*refreshed)->auth.apiKey, "fresh-access");
    EXPECT_EQ(flow.refreshCount(), 1);

    StubOauthFlow other("acme");
    const auto second = m_runtime.registerOauthFlow("acme", other);
    ASSERT_FALSE(second.has_value());
    EXPECT_NE(second.error().message.find("already has an OAuth sign-in"), std::string::npos);
    m_runtime.unregisterOauthFlow("acme", other);
    EXPECT_TRUE(m_runtime.getAuth("acme", std::nullopt, {})->has_value()) << "removing a flow that is not the provider's changes nothing";

    m_runtime.unregisterOauthFlow("acme", flow);
    const auto after = m_runtime.getAuth("acme", std::nullopt, {});
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->has_value());
}
