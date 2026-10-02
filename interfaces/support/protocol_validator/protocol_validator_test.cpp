#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.protocol_validator;

class ProtocolValidatorTest : public testing::Test {
protected:
    bool client(const Json& message) {
        return m_validator.validate(ProtocolSide::Client, message).has_value();
    }

    bool server(const Json& message) {
        return m_validator.validate(ProtocolSide::Server, message).has_value();
    }

    Json request(Json call = Json{{"serviceId", "pi.models"}, {"member", "list"}, {"args", Json::array()}}) {
        return Json{{"type", "request"}, {"id", "request-1"}, {"target", Json{{"serverId", m_serverId}}}, {"call", call}};
    }

    const std::string m_serverId = "00000000-0000-4000-8000-000000000001";
    ProtocolValidator m_validator;
};

TEST_F(ProtocolValidatorTest, NegotiatesProtocolVersionEight) {
    EXPECT_EQ(ProtocolValidator::kProtocolVersion, 8);
    EXPECT_TRUE(m_validator.supportedVersion(8));
    EXPECT_FALSE(m_validator.supportedVersion(7));
    EXPECT_FALSE(m_validator.supportedVersion(8.5));
    for (const int version : {0, 8, 9}) {
        EXPECT_TRUE(client(Json{{"type", "hello"}, {"version", version}}));
    }
}

TEST_F(ProtocolValidatorTest, RejectsInvalidClientHellos) {
    EXPECT_FALSE(client(Json{{"type", "hello"}, {"version", "8"}}));
    EXPECT_FALSE(client(Json{{"type", "hello"}, {"version", 8.5}}));
    EXPECT_FALSE(client(Json{{"type", "hello"}, {"version", 8}, {"extra", true}}));
    EXPECT_FALSE(client(Json{{"type", "hello"}, {"version", -1}}));
    EXPECT_FALSE(client(Json("hello")));
}

TEST_F(ProtocolValidatorTest, RejectsNonCanonicalServerIds) {
    for (const std::string id : {"", "server-1", "00000000-0000-7000-8000-000000000001", "00000000-0000-4000-7000-000000000001",
                                 "00000000-0000-4000-8000-00000000000A"}) {
        Json message = request();
        message["target"]["serverId"] = id;
        EXPECT_FALSE(client(message)) << id;
    }
    EXPECT_TRUE(client(request()));
}

TEST_F(ProtocolValidatorTest, RoutedPayloadsStayOpaque) {
    Json message = request(Json{{"arbitrary", "strict JSON whose meaning belongs to Chord"}});
    message["target"] = Json{{"serverId", m_serverId}, {"sessionId", "session-1"}, {"attachmentId", "attachment-1"}};
    EXPECT_TRUE(client(message));
    EXPECT_TRUE(client(request(nullptr)));
    EXPECT_TRUE(server(Json{{"type", "service_update"}, {"subscriptionId", "s1"}, {"update", Json{{"applicationDefined", true}}}}));
}

TEST_F(ProtocolValidatorTest, RejectsNonJsonOpaquePayloads) {
    for (const Json& bad : {Json::binary({1}), Json(std::numeric_limits<double>::quiet_NaN())}) {
        EXPECT_FALSE(client(request(Json{{"serviceId", "x"}, {"args", Json::array({bad})}})));
        EXPECT_FALSE(server(Json{{"type", "response"}, {"id", "request-1"}, {"ok", true}, {"result", bad}}));
    }
}

TEST_F(ProtocolValidatorTest, ValidatesCancellation) {
    const Json cancel{{"type", "cancel"}, {"id", "request-1"}, {"target", Json{{"serverId", m_serverId}}}};
    EXPECT_TRUE(client(cancel));
    Json emptyId = cancel;
    emptyId["id"] = "";
    EXPECT_FALSE(client(emptyId));
    Json extra = cancel;
    extra["extra"] = true;
    EXPECT_FALSE(client(extra));
}

TEST_F(ProtocolValidatorTest, ValidatesAttachmentUpdates) {
    const Json target{{"serverId", m_serverId}, {"sessionId", "session-1"}, {"attachmentId", "attachment-1"}};
    EXPECT_TRUE(server(Json{{"type", "attachment"}, {"attachment", target}}));
    EXPECT_TRUE(server(Json{{"type", "attachment"}, {"attachment", nullptr}}));
    EXPECT_FALSE(server(Json{{"type", "attachment"}, {"attachment", Json{{"sessionId", "session-1"}}}}));
}

TEST_F(ProtocolValidatorTest, RejectsMalformedRequests) {
    Json emptyId = request();
    emptyId["id"] = "";
    EXPECT_FALSE(client(emptyId));
    Json extra = request();
    extra["extra"] = true;
    EXPECT_FALSE(client(extra));
    Json noCall = request();
    noCall.erase("call");
    EXPECT_FALSE(client(noCall));
}

TEST_F(ProtocolValidatorTest, ValidatesResponses) {
    EXPECT_TRUE(server(Json{{"type", "response"}, {"id", "request-1"}, {"ok", true}}));
    EXPECT_FALSE(server(Json{{"type", "response"}, {"id", "request-1"}, {"ok", true}, {"result", Json::array()}, {"extra", true}}));
    EXPECT_FALSE(server(Json{{"type", "response"}, {"id", "request-1"}, {"ok", false}, {"error", Json{{"code", ""}, {"message", "bad"}}}}));
    for (const char* code : {"wrong_server", "cancelled", "service_not_found", "application_error"}) {
        EXPECT_TRUE(server(Json{{"type", "response"}, {"id", "r"}, {"ok", false}, {"error", Json{{"code", code}, {"message", "safe"}}}}));
    }
}

TEST_F(ProtocolValidatorTest, ValidatesServerHellos) {
    const Json hello{{"type", "hello"}, {"version", 8}, {"serverId", m_serverId}};
    EXPECT_TRUE(server(hello));
    Json badId = hello;
    badId["serverId"] = "server-1";
    EXPECT_FALSE(server(badId));
    Json extra = hello;
    extra["snapshot"] = Json::object();
    EXPECT_FALSE(server(extra));
    Json wrongVersion = hello;
    wrongVersion["version"] = 7;
    EXPECT_FALSE(server(wrongVersion));
    EXPECT_TRUE(server(Json{{"type", "hello_error"}, {"error", Json{{"code", "unsupported_version"}, {"message", "no"}}}}));
    EXPECT_FALSE(server(Json{{"type", "unknown"}, {"event", Json::object()}}));
}

TEST_F(ProtocolValidatorTest, ErrorsNameTheSide) {
    EXPECT_EQ(m_validator.validate(ProtocolSide::Client, Json::object()).error().message, "Invalid client protocol message");
    EXPECT_EQ(m_validator.validate(ProtocolSide::Server, Json::object()).error().message, "Invalid server protocol message");
}
