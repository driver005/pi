#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.model_codec;

TEST(ModelCodecTest, RoundTripsAFullModel) {
    const Json json = Json::parse(R"({
        "id":"claude-sonnet-4","name":"Claude Sonnet 4","api":"anthropic-messages","provider":"anthropic",
        "baseUrl":"https://api.anthropic.com","input":["text","image"],
        "cost":{"input":3,"output":15,"cacheRead":0.3,"cacheWrite":3.75,
                "tiers":[{"inputTokensAbove":200000,"input":6,"output":22.5,"cacheRead":0.6,"cacheWrite":7.5}]},
        "headers":{"x-a":"b"},"reasoning":true,"thinkingLevelMap":{"minimal":null,"xhigh":"max"},
        "promptCache":{"short":300,"long":3600},"contextWindow":200000,"maxTokens":64000,
        "samplingParams":{"top_p":0.9},"compat":{"supportsTemperature":false}})");
    ModelCodec codec;
    const auto model = codec.fromJson(json);
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->provider, "anthropic");
    EXPECT_TRUE(model->reasoning);
    EXPECT_EQ(model->contextWindow, 200000);
    ASSERT_EQ(model->cost.tiers.size(), 1U);
    EXPECT_EQ(model->cost.tiers[0].inputTokensAbove, 200000);
    EXPECT_EQ(model->headers.at("x-a"), "b");
    EXPECT_TRUE(model->thinkingLevelMap["minimal"].is_null());
    const Json again = codec.toJson(*model);
    EXPECT_EQ(again["thinkingLevelMap"], json["thinkingLevelMap"]);
    EXPECT_EQ(again["cost"], json["cost"]);
    EXPECT_EQ(again["compat"], json["compat"]);
    EXPECT_EQ(again["input"], json["input"]);
}

TEST(ModelCodecTest, DefaultsForMinimalModel) {
    ModelCodec codec;
    const auto model = codec.fromJson(Json::parse(R"({"id":"local"})"));
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->name, "local");
    EXPECT_EQ(model->input, (std::vector<std::string>{"text"}));
    EXPECT_FALSE(model->reasoning);
    const Json out = codec.toJson(*model);
    EXPECT_FALSE(out.contains("headers"));
    EXPECT_FALSE(out.contains("compat"));
}

TEST(ModelCodecTest, RejectsNonChatAndInvalid) {
    ModelCodec codec;
    EXPECT_FALSE(codec.fromJson(Json::parse(R"({"id":"x","type":"image"})")).has_value());
    EXPECT_TRUE(codec.fromJson(Json::parse(R"({"id":"x","type":"chat"})")).has_value());
    EXPECT_FALSE(codec.fromJson(Json::parse(R"({"name":"x"})")).has_value());
    EXPECT_FALSE(codec.fromJson(Json::parse("[]")).has_value());
}
