#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.service_state_decoder;
import pi.support.service_state_encoder;

class ServiceStateCodecTest : public testing::Test {
protected:
    Json j(const std::string& text) {
        return Json::parse(text);
    }

    Json snapshot(const std::string& members) {
        return j(R"({"serviceId":"pi.states","mode":"singleton","instances":[{"members":)" + members + "}]}");
    }

    Json stateUpdate(const std::string& member, int sequence, const std::string& ops) {
        return j(R"({"type":"state","member":")" + member + R"(","sequence":)" + std::to_string(sequence) + R"(,"ops":)" + ops + "}");
    }

    ServiceStateEncoder m_encoder;
    ServiceStateDecoder m_decoder;
};

TEST_F(ServiceStateCodecTest, KeepsOneCodecPairPerSubscriptionState) {
    const Json initial = snapshot(R"([{"name":"state","kind":"state","sequence":0,"ops":[["r",{"revision":0}]]}])");
    EXPECT_EQ(*m_decoder.decodeSnapshot(*m_encoder.encodeSnapshot(initial)), initial);
    const Json first = stateUpdate("state", 1, R"([["s",["revision"],1]])");
    const Json second = stateUpdate("state", 2, R"([["s",["revision"],2]])");
    const Json firstWire = *m_encoder.encodeUpdate(first);
    const Json secondWire = *m_encoder.encodeUpdate(second);
    EXPECT_EQ(firstWire["ops"], j(R"([["s",["revision"],1]])"));
    EXPECT_EQ(secondWire["ops"], j(R"([["#",0,["revision"]],["s",0,2]])"));
    EXPECT_EQ(*m_decoder.decodeUpdate(firstWire), first);
    EXPECT_EQ(*m_decoder.decodeUpdate(secondWire), second);
}

TEST_F(ServiceStateCodecTest, ResetsRestartPathDictionariesAtTheNewBaseline) {
    const Json initial = snapshot(R"([{"name":"state","kind":"state","sequence":0,"ops":[["r",{"before":0}]]}])");
    m_decoder.decodeSnapshot(*m_encoder.encodeSnapshot(initial));
    for (int sequence = 1; sequence <= 2; ++sequence) {
        ASSERT_TRUE(m_decoder.decodeUpdate(*m_encoder.encodeUpdate(stateUpdate("state", sequence, R"([["s",["before"],1]])"))).has_value());
    }
    const Json reset = j(R"({"type":"reset","snapshot":{"serviceId":"pi.states","mode":"singleton","instances":[{"members":[{"name":"state","kind":"state","sequence":103,"ops":[["r",{"after":103}]]}]}]}})");
    const Json wireReset = *m_encoder.encodeUpdate(reset);
    EXPECT_EQ(*m_decoder.decodeUpdate(wireReset), reset);
    for (int sequence = 104; sequence <= 106; ++sequence) {
        const Json update = stateUpdate("state", sequence, R"([["s",["after"],5]])");
        EXPECT_EQ(*m_decoder.decodeUpdate(*m_encoder.encodeUpdate(update)), update);
    }
}

TEST_F(ServiceStateCodecTest, IsolatesDictionariesBetweenStatesAndSubscriptions) {
    const Json initial = snapshot(R"([{"name":"left","kind":"state","sequence":0,"ops":[["r",{"revision":0}]]},{"name":"right","kind":"state","sequence":0,"ops":[["r",{"revision":0}]]}])");
    ServiceStateEncoder other;
    ServiceStateDecoder otherDecoder;
    m_decoder.decodeSnapshot(*m_encoder.encodeSnapshot(initial));
    otherDecoder.decodeSnapshot(*other.encodeSnapshot(initial));
    const auto update = [this](const std::string& member, int sequence) {
        return stateUpdate(member, sequence, R"([["s",["revision"],)" + std::to_string(sequence) + "]]");
    };
    EXPECT_EQ((*m_encoder.encodeUpdate(update("left", 1)))["ops"], j(R"([["s",["revision"],1]])"));
    EXPECT_EQ((*m_encoder.encodeUpdate(update("right", 1)))["ops"], j(R"([["s",["revision"],1]])"));
    EXPECT_EQ((*m_encoder.encodeUpdate(update("left", 2)))["ops"], j(R"([["#",0,["revision"]],["s",0,2]])"));
    EXPECT_EQ((*m_encoder.encodeUpdate(update("right", 2)))["ops"], j(R"([["#",0,["revision"]],["s",0,2]])"));
    // The second subscription has not seen any of this traffic: its first update is still inline.
    EXPECT_EQ((*other.encodeUpdate(update("left", 1)))["ops"], j(R"([["s",["revision"],1]])"));
}

TEST_F(ServiceStateCodecTest, KeyedInstancesComeAndGo) {
    const Json spawned = j(R"({"type":"spawned","instance":{"instance":{"key":"a","generation":1},"members":[{"name":"state","kind":"state","sequence":0,"ops":[["r",{"n":0}]]}]}})");
    const Json empty = j(R"({"serviceId":"pi.k","mode":"keyed","instances":[]})");
    ASSERT_TRUE(m_encoder.encodeSnapshot(empty).has_value());
    ASSERT_TRUE(m_decoder.decodeSnapshot(empty).has_value());
    EXPECT_EQ(*m_decoder.decodeUpdate(*m_encoder.encodeUpdate(spawned)), spawned);
    const Json update = j(R"({"type":"state","instance":{"key":"a","generation":1},"member":"state","sequence":1,"ops":[["s",["n"],1]]})");
    EXPECT_EQ(*m_decoder.decodeUpdate(*m_encoder.encodeUpdate(update)), update);
    const Json closed = j(R"({"type":"closed","instance":{"key":"a","generation":1}})");
    EXPECT_EQ(*m_encoder.encodeUpdate(closed), closed);
    EXPECT_FALSE(m_encoder.encodeUpdate(update).has_value());
    // A new generation may reuse the key.
    const Json respawned = j(R"({"type":"spawned","instance":{"instance":{"key":"a","generation":2},"members":[{"name":"state","kind":"state","sequence":0,"ops":[["r",{"n":0}]]}]}})");
    EXPECT_TRUE(m_encoder.encodeUpdate(respawned).has_value());
}

TEST_F(ServiceStateCodecTest, ReportsUnknownAndDuplicateStates) {
    const Json initial = snapshot(R"([{"name":"state","kind":"state","sequence":0,"ops":[["r",{}]]}])");
    m_encoder.encodeSnapshot(initial);
    const auto unknown = m_encoder.encodeUpdate(stateUpdate("other", 1, R"([["s",["a"],1]])"));
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().message, "Unknown service state other");
    const auto duplicate = m_encoder.encodeSnapshot(snapshot(R"([{"name":"s","kind":"state","sequence":0,"ops":[["r",1]]},{"name":"s","kind":"state","sequence":0,"ops":[["r",1]]}])"));
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().message, "Duplicate service state s");
}

TEST_F(ServiceStateCodecTest, UnavailableAndReplacedResetTheDictionaries) {
    m_encoder.encodeSnapshot(snapshot(R"([{"name":"state","kind":"state","sequence":0,"ops":[["r",{"a":0}]]}])"));
    EXPECT_EQ(*m_encoder.encodeUpdate(j(R"({"type":"unavailable"})")), j(R"({"type":"unavailable"})"));
    EXPECT_FALSE(m_encoder.encodeUpdate(stateUpdate("state", 1, R"([["s",["a"],1]])")).has_value());
    const Json replaced = j(R"({"type":"replaced","snapshot":{"members":[{"name":"state","kind":"state","sequence":3,"ops":[["r",{"a":3}]]}]}})");
    ASSERT_TRUE(m_encoder.encodeUpdate(replaced).has_value());
    EXPECT_TRUE(m_encoder.encodeUpdate(stateUpdate("state", 4, R"([["s",["a"],4]])")).has_value());
    EXPECT_FALSE(m_encoder.encodeUpdate(j(R"({"type":"mystery"})")).has_value());
}
