#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.mcp_message_codec;

class McpMessageCodecTest : public testing::Test {
protected:
    McpMessageCodec m_codec;
};

TEST_F(McpMessageCodecTest, BuildsMessages) {
    EXPECT_EQ(m_codec.request(3, "tools/list", Json()), Json::parse(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})"));
    EXPECT_EQ(m_codec.request(1, "x", Json{{"a", 1}})["params"]["a"], 1);
    EXPECT_FALSE(m_codec.notification("notifications/initialized", Json()).contains("params"));
    EXPECT_EQ(m_codec.result(Json(7), Json())["result"], Json::object());
    EXPECT_EQ(m_codec.error(Json("a"), McpMessageCodec::MethodNotFound, "nope")["error"]["code"], -32601);
}

TEST_F(McpMessageCodecTest, ClassifiesMessages) {
    EXPECT_TRUE(m_codec.isRequest(Json::parse(R"({"jsonrpc":"2.0","id":1,"method":"ping"})")));
    EXPECT_TRUE(m_codec.isRequest(Json::parse(R"({"jsonrpc":"2.0","id":"s","method":"ping"})")));
    EXPECT_FALSE(m_codec.isRequest(Json::parse(R"({"jsonrpc":"2.0","id":null,"method":"ping"})")));
    EXPECT_FALSE(m_codec.isRequest(Json::parse(R"({"jsonrpc":"1.0","id":1,"method":"ping"})")));
    EXPECT_TRUE(m_codec.isNotification(Json::parse(R"({"jsonrpc":"2.0","method":"notifications/x"})")));
    EXPECT_FALSE(m_codec.isNotification(Json::parse(R"({"jsonrpc":"2.0","id":1,"method":"x"})")));
    EXPECT_TRUE(m_codec.isResponse(Json::parse(R"({"jsonrpc":"2.0","id":1,"result":{}})")));
    EXPECT_TRUE(m_codec.isResponse(Json::parse(R"({"jsonrpc":"2.0","id":1,"error":{"code":-1,"message":"m"}})")));
    EXPECT_FALSE(m_codec.isResponse(Json::parse(R"({"jsonrpc":"2.0","id":1,"result":1,"error":{"code":1,"message":"m"}})")));
    EXPECT_FALSE(m_codec.isResponse(Json::parse(R"({"jsonrpc":"2.0","id":1,"error":{"code":"x","message":"m"}})")));
    EXPECT_FALSE(m_codec.isResponse(Json::parse(R"({"jsonrpc":"2.0","id":1})")));
    EXPECT_FALSE(m_codec.isResponse(Json("text")));
}

TEST_F(McpMessageCodecTest, RpcErrorCodesRoundTrip) {
    EXPECT_EQ(m_codec.rpcCode(-32601), "rpc:-32601");
    EXPECT_EQ(m_codec.rpcNumber("rpc:-32601"), -32601);
    EXPECT_FALSE(m_codec.rpcNumber("timeout").has_value());
    EXPECT_FALSE(m_codec.rpcNumber("rpc:abc").has_value());
}
