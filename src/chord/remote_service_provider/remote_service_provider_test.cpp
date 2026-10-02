#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.chord.remote_service_provider;
import pi.chord.replicated_state;

class TestService : public IRemoteService {
public:
    explicit TestService(Json initial = Json{{"n", 0}}) : m_state(std::move(initial)) {}

    std::map<std::string, Method> methods() override {
        return {{"echo", [](const std::vector<Json>& args, const ServiceContext&) -> Result<std::optional<Json>> {
                     return std::optional<Json>(args.empty() ? Json() : args[0]);
                 }},
                {"nothing", [](const std::vector<Json>&, const ServiceContext&) -> Result<std::optional<Json>> {
                     return std::optional<Json>();
                 }},
                {"fail", [](const std::vector<Json>&, const ServiceContext&) -> Result<std::optional<Json>> {
                     return std::unexpected(Error{"application_error", "it failed"});
                 }}};
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {{"state", &m_state}};
    }

    void set(int n) {
        m_state.change(ServiceContext{}, [n](Json& draft) { draft["n"] = n; });
    }

    ReplicatedState m_state;
};

class OtherShapeService : public IRemoteService {
public:
    std::map<std::string, Method> methods() override {
        return {{"only", [](const std::vector<Json>&, const ServiceContext&) -> Result<std::optional<Json>> { return std::optional<Json>(); }}};
    }

    std::map<std::string, IReplicatedState*> states() override {
        return {};
    }
};

class RemoteServiceProviderTest : public testing::Test {
protected:
    RemoteServiceProviderTest()
        : m_provider({ServiceDefinition{"pi.single", "singleton"}, ServiceDefinition{"pi.many", "keyed"},
                      ServiceDefinition{"pi.other", "singleton"}}) {}

    Json call(const std::string& service, const std::string& member, Json args = Json::array(), Json instance = Json()) {
        Json out{{"serviceId", service}, {"member", member}, {"args", args}};
        if (!instance.is_null()) {
            out["instance"] = instance;
        }
        return out;
    }

    ServiceSubscription follow(const std::string& service, const std::string& mode, std::vector<Json>& updates) {
        auto subscription = m_provider.subscribe(service, mode, [&updates, this](const Json& update, const ServiceContext&) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            updates.push_back(update);
        });
        EXPECT_TRUE(subscription.has_value());
        return *subscription;
    }

    std::string errorCode(const Result<std::optional<Json>>& result) {
        return result ? "" : result.error().code;
    }

    RemoteServiceProvider m_provider;
    std::mutex m_mutex;
};

TEST_F(RemoteServiceProviderTest, ListsItsCatalogue) {
    EXPECT_EQ(m_provider.catalogue(), Json::parse(R"([{"serviceId":"pi.single","mode":"singleton"},{"serviceId":"pi.many","mode":"keyed"},{"serviceId":"pi.other","mode":"singleton"}])"));
}

TEST_F(RemoteServiceProviderTest, InvokesMethodsOfProvidedSingletons) {
    auto service = std::make_shared<TestService>();
    ASSERT_TRUE(m_provider.provide("pi.single", service).has_value());
    const auto echoed = m_provider.invoke(call("pi.single", "echo", Json::array({Json{{"a", 1}}})), ServiceContext{});
    ASSERT_TRUE(echoed.has_value());
    EXPECT_EQ(**echoed, (Json{{"a", 1}}));
    const auto nothing = m_provider.invoke(call("pi.single", "nothing"), ServiceContext{});
    ASSERT_TRUE(nothing.has_value());
    EXPECT_FALSE(nothing->has_value());
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.single", "fail"), ServiceContext{})), "application_error");
}

TEST_F(RemoteServiceProviderTest, ReportsRoutingErrors) {
    ASSERT_TRUE(m_provider.provide("pi.single", std::make_shared<TestService>()).has_value());
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.unknown", "echo"), ServiceContext{})), "service_not_allowed");
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.other", "echo"), ServiceContext{})), "service_not_found");
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.single", "missing"), ServiceContext{})), "service_member_not_found");
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.single", "state"), ServiceContext{})), "service_member_mismatch");
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.single", "echo", Json::array(), Json{{"key", "a"}, {"generation", 1}}), ServiceContext{})), "service_mode_mismatch");
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.many", "echo"), ServiceContext{})), "service_mode_mismatch");
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.many", "echo", Json::array(), Json{{"key", "a"}, {"generation", 1}}), ServiceContext{})), "service_instance_not_found");
}

TEST_F(RemoteServiceProviderTest, ProvideValidatesItsArguments) {
    EXPECT_EQ(m_provider.provide("pi.unknown", std::make_shared<TestService>()).error().code, "service_not_allowed");
    EXPECT_EQ(m_provider.provide("pi.many", std::make_shared<TestService>()).error().code, "service_mode_mismatch");
    EXPECT_EQ(m_provider.provide("pi.single", nullptr).error().code, "service_invalid_value");
    ASSERT_TRUE(m_provider.provide("pi.single", std::make_shared<TestService>()).has_value());
    EXPECT_EQ(m_provider.provide("pi.single", std::make_shared<TestService>()).error().code, "service_mode_mismatch");
}

TEST_F(RemoteServiceProviderTest, SubscriptionsStartWithAnAtomicSnapshot) {
    auto service = std::make_shared<TestService>(Json{{"n", 7}});
    ASSERT_TRUE(m_provider.provide("pi.single", service).has_value());
    service->set(8);
    std::vector<Json> updates;
    const ServiceSubscription subscription = follow("pi.single", "singleton", updates);
    EXPECT_EQ(subscription.snapshot,
              Json::parse(R"({"serviceId":"pi.single","mode":"singleton","instances":[{"members":[{"name":"echo","kind":"method"},{"name":"fail","kind":"method"},{"name":"nothing","kind":"method"},{"name":"state","kind":"state","sequence":1,"ops":[["r",{"n":8}]]}]}]})"));
}

TEST_F(RemoteServiceProviderTest, UpdatesBufferUntilActivationAndKeepOrder) {
    auto service = std::make_shared<TestService>();
    ASSERT_TRUE(m_provider.provide("pi.single", service).has_value());
    std::vector<Json> updates;
    ServiceSubscription subscription = follow("pi.single", "singleton", updates);
    service->set(1);
    service->set(2);
    EXPECT_TRUE(updates.empty());
    subscription.activate();
    ASSERT_EQ(updates.size(), 2U);
    EXPECT_EQ(updates[0], Json::parse(R"({"type":"state","member":"state","sequence":1,"ops":[["s",["n"],1]]})"));
    EXPECT_EQ(updates[1]["sequence"], 2);
    service->set(3);
    ASSERT_EQ(updates.size(), 3U);
    EXPECT_EQ(updates[2]["sequence"], 3);
}

TEST_F(RemoteServiceProviderTest, OverflowBeforeActivationBecomesAFullReset) {
    auto service = std::make_shared<TestService>();
    ASSERT_TRUE(m_provider.provide("pi.single", service).has_value());
    std::vector<Json> updates;
    ServiceSubscription subscription = follow("pi.single", "singleton", updates);
    for (int i = 1; i <= 101; ++i) {
        service->set(i);
    }
    subscription.activate();
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0]["type"], "reset");
    EXPECT_EQ(updates[0]["snapshot"]["instances"][0]["members"][3]["sequence"], 101);
    EXPECT_EQ(updates[0]["snapshot"]["instances"][0]["members"][3]["ops"][0][1]["n"], 101);
    service->set(500);
    ASSERT_EQ(updates.size(), 2U);
    EXPECT_EQ(updates[1]["sequence"], 102);
}

TEST_F(RemoteServiceProviderTest, ClosedSubscriptionsHearNothing) {
    auto service = std::make_shared<TestService>();
    ASSERT_TRUE(m_provider.provide("pi.single", service).has_value());
    std::vector<Json> updates;
    ServiceSubscription subscription = follow("pi.single", "singleton", updates);
    subscription.activate();
    subscription.close();
    subscription.close();
    service->set(1);
    EXPECT_TRUE(updates.empty());
}

TEST_F(RemoteServiceProviderTest, SubscribeValidatesTheServiceAndMode) {
    EXPECT_EQ(m_provider.subscribe("pi.unknown", "singleton", nullptr).error().code, "service_not_allowed");
    EXPECT_EQ(m_provider.subscribe("pi.single", "keyed", nullptr).error().code, "service_mode_mismatch");
    EXPECT_EQ(m_provider.subscribe("pi.single", "singleton", nullptr).error().code, "service_not_found");
}

TEST_F(RemoteServiceProviderTest, WithdrawAndReplaceNotifySubscribers) {
    auto first = std::make_shared<TestService>();
    ASSERT_TRUE(m_provider.provide("pi.single", first).has_value());
    std::vector<Json> updates;
    ServiceSubscription subscription = follow("pi.single", "singleton", updates);
    subscription.activate();
    auto second = std::make_shared<TestService>(Json{{"n", 40}});
    ASSERT_TRUE(m_provider.replace("pi.single", second).has_value());
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0]["type"], "replaced");
    EXPECT_EQ(updates[0]["snapshot"]["members"][3]["ops"][0][1]["n"], 40);
    first->set(9);
    EXPECT_EQ(updates.size(), 1U);
    second->set(41);
    ASSERT_EQ(updates.size(), 2U);
    EXPECT_EQ(updates[1]["type"], "state");
    ASSERT_TRUE(m_provider.withdraw("pi.single").has_value());
    ASSERT_EQ(updates.size(), 3U);
    EXPECT_EQ(updates[2]["type"], "unavailable");
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.single", "echo"), ServiceContext{})), "service_not_found");
    ASSERT_TRUE(m_provider.withdraw("pi.single").has_value());
}

TEST_F(RemoteServiceProviderTest, ReplacementsMustKeepTheMemberShape) {
    ASSERT_TRUE(m_provider.provide("pi.single", std::make_shared<TestService>()).has_value());
    EXPECT_EQ(m_provider.replace("pi.single", std::make_shared<OtherShapeService>()).error().code, "service_member_mismatch");
}

TEST_F(RemoteServiceProviderTest, KeyedInstancesSpawnAndClose) {
    std::vector<Json> updates;
    ServiceSubscription subscription = follow("pi.many", "keyed", updates);
    EXPECT_EQ(subscription.snapshot["instances"], Json::array());
    subscription.activate();
    auto first = std::make_shared<TestService>();
    auto closeFirst = m_provider.spawn("pi.many", "a", first);
    ASSERT_TRUE(closeFirst.has_value());
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0]["type"], "spawned");
    EXPECT_EQ(updates[0]["instance"]["instance"], (Json{{"key", "a"}, {"generation", 1}}));
    EXPECT_EQ(m_provider.spawn("pi.many", "a", std::make_shared<TestService>()).error().code, "service_mode_mismatch");
    EXPECT_EQ(m_provider.spawn("pi.many", "", std::make_shared<TestService>()).error().code, "service_invalid_value");
    EXPECT_EQ(m_provider.spawn("pi.single", "a", std::make_shared<TestService>()).error().code, "service_mode_mismatch");

    const Json address{{"key", "a"}, {"generation", 1}};
    ASSERT_TRUE(m_provider.invoke(call("pi.many", "echo", Json::array({1}), address), ServiceContext{}).has_value());
    first->set(5);
    ASSERT_EQ(updates.size(), 2U);
    EXPECT_EQ(updates[1]["instance"], address);
    EXPECT_EQ(updates[1]["member"], "state");

    (*closeFirst)();
    (*closeFirst)();
    ASSERT_EQ(updates.size(), 3U);
    EXPECT_EQ(updates[2], Json::parse(R"({"type":"closed","instance":{"key":"a","generation":1}})"));
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.many", "echo", Json::array(), address), ServiceContext{})), "service_instance_not_found");

    auto second = m_provider.spawn("pi.many", "a", std::make_shared<TestService>());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(updates[3]["instance"]["instance"]["generation"], 2);
    EXPECT_EQ(errorCode(m_provider.invoke(call("pi.many", "echo", Json::array(), address), ServiceContext{})), "service_stale_instance");
}

TEST_F(RemoteServiceProviderTest, KeyedSnapshotsListInstancesInKeyOrder) {
    ASSERT_TRUE(m_provider.spawn("pi.many", "b", std::make_shared<TestService>()).has_value());
    ASSERT_TRUE(m_provider.spawn("pi.many", "a", std::make_shared<TestService>()).has_value());
    std::vector<Json> updates;
    const ServiceSubscription subscription = follow("pi.many", "keyed", updates);
    ASSERT_EQ(subscription.snapshot["instances"].size(), 2U);
    EXPECT_EQ(subscription.snapshot["instances"][0]["instance"]["key"], "a");
    EXPECT_EQ(subscription.snapshot["instances"][1]["instance"]["key"], "b");
}

TEST_F(RemoteServiceProviderTest, DisposeStopsEverything) {
    auto service = std::make_shared<TestService>();
    ASSERT_TRUE(m_provider.provide("pi.single", service).has_value());
    std::vector<Json> updates;
    ServiceSubscription subscription = follow("pi.single", "singleton", updates);
    subscription.activate();
    m_provider.dispose();
    m_provider.dispose();
    service->set(1);
    EXPECT_TRUE(updates.empty());
    EXPECT_EQ(m_provider.provide("pi.other", std::make_shared<TestService>()).error().code, "disposed");
    EXPECT_FALSE(m_provider.invoke(call("pi.single", "echo"), ServiceContext{}).has_value());
}

TEST_F(RemoteServiceProviderTest, ConcurrentPublishersDeliverEveryUpdateInOrder) {
    auto service = std::make_shared<TestService>(Json{{"n", 0}});
    ASSERT_TRUE(m_provider.provide("pi.single", service).has_value());
    std::vector<Json> updates;
    ServiceSubscription subscription = follow("pi.single", "singleton", updates);
    subscription.activate();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 25; ++i) {
                service->m_state.change(ServiceContext{}, [](Json& draft) { draft["n"] = draft["n"].get<int>() + 1; });
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    ASSERT_EQ(updates.size(), 100U);
    for (std::size_t i = 0; i < updates.size(); ++i) {
        EXPECT_EQ(updates[i]["sequence"], static_cast<int>(i) + 1);
    }
}
