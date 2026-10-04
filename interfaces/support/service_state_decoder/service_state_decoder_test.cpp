#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.service_state_decoder;
import pi.support.service_state_encoder;

TEST(ServiceStateDecoderTest, DecodesWhatTheEncoderProduces) {
    ServiceStateEncoder encoder;
    ServiceStateDecoder decoder;
    const Json snapshot = Json::parse(R"({"serviceId":"s","mode":"singleton","instances":[{"members":[{"name":"state","kind":"state","sequence":0,"ops":[["r",{"a":0}]]},{"name":"call","kind":"method"}]}]})");
    EXPECT_EQ(*decoder.decodeSnapshot(*encoder.encodeSnapshot(snapshot)), snapshot);
    for (int sequence = 1; sequence <= 3; ++sequence) {
        const Json update{{"type", "state"}, {"member", "state"}, {"sequence", sequence},
                          {"ops", Json::array({Json::array({"s", Json::array({"a"}), sequence})})}};
        EXPECT_EQ(*decoder.decodeUpdate(*encoder.encodeUpdate(update)), update);
    }
}

TEST(ServiceStateDecoderTest, RejectsUpdatesForUnknownStatesAndUnknownTypes) {
    ServiceStateDecoder decoder;
    EXPECT_FALSE(decoder.decodeUpdate(Json::parse(R"({"type":"state","member":"x","sequence":1,"ops":[]})")).has_value());
    EXPECT_FALSE(decoder.decodeUpdate(Json::parse(R"({"type":"mystery"})")).has_value());
    const Json snapshot = Json::parse(R"({"serviceId":"s","mode":"singleton","instances":[{"members":[{"name":"state","kind":"state","sequence":0,"ops":[["r",1]]}]}]})");
    ASSERT_TRUE(decoder.decodeSnapshot(snapshot).has_value());
    EXPECT_FALSE(decoder.decodeUpdate(Json::parse(R"({"type":"state","member":"state","sequence":1,"ops":[["a","x"]]})")).has_value());
}
