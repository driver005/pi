#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.service_wire;

class ServiceWireTest : public testing::Test {
protected:
    Json j(const std::string& text) {
        return Json::parse(text);
    }

    ServiceWire m_wire;
};

TEST_F(ServiceWireTest, EncodesAndDecodesControlCalls) {
    EXPECT_EQ(m_wire.decodeControl(m_wire.catalogueCall())->type, "catalogue");
    const auto subscribe = m_wire.decodeControl(m_wire.subscribeCall("subscription-1", "pi.models", "singleton"));
    ASSERT_TRUE(subscribe.has_value());
    EXPECT_EQ(subscribe->type, "subscribe");
    EXPECT_EQ(subscribe->subscriptionId, "subscription-1");
    EXPECT_EQ(subscribe->serviceId, "pi.models");
    EXPECT_EQ(subscribe->mode, "singleton");
    const auto unsubscribe = m_wire.decodeControl(m_wire.unsubscribeCall("subscription-1"));
    ASSERT_TRUE(unsubscribe.has_value());
    EXPECT_EQ(unsubscribe->type, "unsubscribe");
    EXPECT_EQ(unsubscribe->subscriptionId, "subscription-1");
}

TEST_F(ServiceWireTest, OrdinaryAndMalformedCallsAreNotControlCalls) {
    EXPECT_FALSE(m_wire.decodeControl(j(R"({"serviceId":"pi.models","member":"list","args":[]})")).has_value());
    EXPECT_FALSE(m_wire.decodeControl(j(R"({"serviceId":"$chord.service","member":"subscribe","args":["s","x","weird"]})")).has_value());
    EXPECT_FALSE(m_wire.decodeControl(j(R"({"serviceId":"$chord.service","member":"catalogue","args":[1]})")).has_value());
    EXPECT_FALSE(m_wire.decodeControl(j(R"({"serviceId":"$chord.service","instance":{"key":"a","generation":1},"member":"catalogue","args":[]})")).has_value());
}

TEST_F(ServiceWireTest, ValidatesServiceCallsAndCatalogues) {
    EXPECT_TRUE(m_wire.validateCall(j(R"({"serviceId":"pi.question-dialog","instance":{"key":"invocation-1","generation":2},"member":"submit","args":[{"outcome":"selected","index":0}]})")).has_value());
    const auto extra = m_wire.validateCall(j(R"({"serviceId":"pi.models","member":"list","args":[],"extra":true})"));
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error().message, "Invalid service call");
    EXPECT_FALSE(m_wire.validateCall(j(R"({"serviceId":"","member":"list","args":[]})")).has_value());
    EXPECT_FALSE(m_wire.validateCall(j(R"({"serviceId":"x","member":"list","args":{}})")).has_value());
    EXPECT_FALSE(m_wire.validateCall(j(R"({"serviceId":"x","member":"list","args":[],"instance":{"key":"a","generation":0}})")).has_value());

    EXPECT_TRUE(m_wire.validateCatalogue(j(R"([{"serviceId":"pi.models","mode":"singleton"},{"serviceId":"pi.dialogs","mode":"keyed"}])")).has_value());
    EXPECT_FALSE(m_wire.validateCatalogue(j(R"([{"serviceId":"pi.models","mode":"unknown"}])")).has_value());
    EXPECT_FALSE(m_wire.validateCatalogue(j(R"([{"serviceId":"a","mode":"keyed"},{"serviceId":"a","mode":"keyed"}])")).has_value());
}

TEST_F(ServiceWireTest, ValidatesDecodedAndWireSnapshotsAndUpdates) {
    const Json snapshot = j(R"({"serviceId":"pi.models","mode":"singleton","instances":[{"members":[{"name":"state","kind":"state","sequence":0,"ops":[["r",{"revision":1}]]},{"name":"call","kind":"method"}]}]})");
    EXPECT_TRUE(m_wire.validateSubscriptionSnapshot(snapshot, false).has_value());
    EXPECT_TRUE(m_wire.validateSubscriptionSnapshot(snapshot, true).has_value());
    const Json update = j(R"({"type":"state","member":"state","sequence":1,"ops":[["s",["revision"],2]]})");
    EXPECT_TRUE(m_wire.validateUpdate(update, false).has_value());
    const Json shortForm = j(R"({"type":"state","member":"state","sequence":2,"ops":[["s",2]]})");
    EXPECT_FALSE(m_wire.validateUpdate(shortForm, false).has_value());
    EXPECT_TRUE(m_wire.validateUpdate(shortForm, true).has_value());
    EXPECT_FALSE(m_wire.validateUpdate(j(R"({"type":"state","member":"state","sequence":0,"ops":[]})"), false).has_value());
    EXPECT_FALSE(m_wire.validateUpdate(j(R"({"type":"state","member":"state","sequence":1,"ops":[["?",0]]})"), true).has_value());
}

TEST_F(ServiceWireTest, ValidatesTheOtherUpdateKinds) {
    const std::string instance = R"({"instance":{"key":"k","generation":1},"members":[]})";
    EXPECT_TRUE(m_wire.validateUpdate(j(R"({"type":"unavailable"})"), false).has_value());
    EXPECT_FALSE(m_wire.validateUpdate(j(R"({"type":"unavailable","extra":1})"), false).has_value());
    EXPECT_TRUE(m_wire.validateUpdate(j(R"({"type":"replaced","snapshot":)" + instance + "}"), false).has_value());
    EXPECT_TRUE(m_wire.validateUpdate(j(R"({"type":"spawned","instance":)" + instance + "}"), false).has_value());
    EXPECT_TRUE(m_wire.validateUpdate(j(R"({"type":"closed","instance":{"key":"k","generation":1}})"), false).has_value());
    EXPECT_FALSE(m_wire.validateUpdate(j(R"({"type":"closed","instance":{"key":"k"}})"), false).has_value());
    EXPECT_FALSE(m_wire.validateUpdate(j(R"({"type":"mystery"})"), false).has_value());
    EXPECT_FALSE(m_wire.validateUpdate(j("[]"), false).has_value());
}

TEST_F(ServiceWireTest, ResetsMustCarryFullRootReplacements) {
    const std::string base = R"({"serviceId":"pi.states","mode":"singleton","instances":[{"members":[{"name":"state","kind":"state","sequence":103,"ops":)";
    EXPECT_TRUE(m_wire.validateUpdate(j(R"({"type":"reset","snapshot":)" + base + R"([["r",{"after":103}]]}]}]}})"), false).has_value());
    const auto partial = m_wire.validateUpdate(j(R"({"type":"reset","snapshot":)" + base + R"([["s",["before"],103]]}]}]}})"), false);
    ASSERT_FALSE(partial.has_value());
    EXPECT_NE(partial.error().message.find("full root replacements"), std::string::npos);
    EXPECT_FALSE(m_wire.validateUpdate(j(R"({"type":"reset","snapshot":{}})"), false).has_value());
    EXPECT_FALSE(m_wire.validateUpdate(j(R"({"type":"reset","snapshot":)" + base + R"([["r",1]]}]}]},"extra":true})"), false).has_value());
}
