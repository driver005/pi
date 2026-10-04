#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.eval_observation_codec;

TEST(EvalObservationCodecTest, RoundTripsAnObservationWithItsMetrics) {
    const EvalObservationCodec codec;
    EvalObservation observation;
    observation.evalSet = "Set";
    observation.caseId = "case";
    observation.variant = "with_docs";
    observation.model = "p/m";
    observation.runNumber = 2;
    observation.outcome = "scored";
    observation.score = 1;
    observation.totalTokens = 120;
    observation.estimatedCostUsd = 0.0125;
    const Json json = codec.toJson(observation);
    EXPECT_EQ(json["outcome"], "scored");
    EXPECT_EQ(json["score"], 1);
    EXPECT_EQ(json["totalTokens"], 120);
    EXPECT_FALSE(json.contains("toolCalls"));
    const auto back = codec.fromJson(json);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->runNumber, 2);
    EXPECT_EQ(back->score, 1);
    EXPECT_EQ(back->totalTokens, 120);
    EXPECT_EQ(back->estimatedCostUsd, 0.0125);
    EXPECT_FALSE(back->toolCalls.has_value());
}

TEST(EvalObservationCodecTest, RejectsIncompleteJson) {
    const EvalObservationCodec codec;
    EXPECT_FALSE(codec.fromJson(Json::object()).has_value());
    EXPECT_FALSE(codec.fromJson(Json::parse(R"({"evalSet":"a","caseId":"b","variant":"c","model":"d","outcome":"scored"})")).has_value()) << "no run number";
    EXPECT_FALSE(codec.fromJson(Json(3)).has_value());
}
