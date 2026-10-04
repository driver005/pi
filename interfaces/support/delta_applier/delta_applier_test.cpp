#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.delta_applier;

class DeltaApplierTest : public testing::Test {
protected:
    Json applied(const std::string& target, const std::string& ops) {
        const auto result = m_applier.apply(Json::parse(target), Json::parse(ops));
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return result ? *result : Json();
    }

    bool fails(const std::string& target, const std::string& ops) {
        return !m_applier.apply(Json::parse(target), Json::parse(ops)).has_value();
    }

    DeltaApplier m_applier;
};

TEST_F(DeltaApplierTest, AppliesAMixedBatch) {
    EXPECT_EQ(applied(R"({"text":"a","values":[1,2],"nested":{"value":1},"stable":{"value":9}})",
                      R"([["a",["text"],"b"],["p",["values"],1,1,[3,4]],["s",["nested","value"],2]])"),
              Json::parse(R"({"text":"ab","values":[1,3,4],"nested":{"value":2},"stable":{"value":9}})"));
}

TEST_F(DeltaApplierTest, DoesNotMutateItsInput) {
    const Json base = Json::parse(R"({"a":{"b":1}})");
    const auto result = m_applier.apply(base, Json::parse(R"([["s",["a","b"],2]])"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(base["a"]["b"], 1);
    EXPECT_EQ((*result)["a"]["b"], 2);
}

TEST_F(DeltaApplierTest, SupportsRootReplacementRootSpliceAndPermutation) {
    Json value = applied("null", R"([["r",[1,2,3]]])");
    value = *m_applier.apply(value, Json::parse(R"([["p",[],1,1,[4]]])"));
    value = *m_applier.apply(value, Json::parse(R"([["m",[],[2,0,1]]])"));
    EXPECT_EQ(value, Json::parse("[3,1,4]"));
}

TEST_F(DeltaApplierTest, RejectsUnsafeAndMalformedPaths) {
    EXPECT_TRUE(fails("{}", R"([["s",["constructor","prototype","x"],true]])"));
    EXPECT_TRUE(fails(R"({"values":[1]})", R"([["s",["values",3],2]])"));
    EXPECT_TRUE(fails(R"({"value":1})", R"([["a",["value"],"x"]])"));
    EXPECT_TRUE(fails("{}", R"([["s","value",1]])"));
    EXPECT_TRUE(fails("{}", R"([["m",[],[0,0]]])"));
    EXPECT_TRUE(fails("{}", R"([["ZZZ",["value"],9]])"));
    EXPECT_TRUE(fails("{}", "[null]"));
    EXPECT_FALSE(m_applier.apply(Json::object(), Json::object()).has_value());
}

TEST_F(DeltaApplierTest, ArrayWritesHitExistingIndicesOrAppendOnePastTheEnd) {
    EXPECT_EQ(applied(R"({"values":[1,2,3]})", R"([["s",["values",1],9]])"), Json::parse(R"({"values":[1,9,3]})"));
    EXPECT_EQ(applied(R"({"values":[1,2,3]})", R"([["s",["values",3],9]])"), Json::parse(R"({"values":[1,2,3,9]})"));
    EXPECT_TRUE(fails(R"({"values":[1,2,3]})", R"([["s",["values",5],9]])"));
    EXPECT_TRUE(fails(R"({"values":[]})", R"([["s",["values",4294967290],1]])"));
    EXPECT_TRUE(fails(R"({"values":[1]})", R"([["s",["values","0"],9]])"));
    EXPECT_TRUE(fails(R"({"values":["a"]})", R"([["a",["values","0"],"b"]])"));
}

TEST_F(DeltaApplierTest, AllowsExplicitGrowthAndRejectsDeletionPastTheEnd) {
    EXPECT_EQ(applied(R"({"values":[1]})", R"([["p",["values"],1,0,[null,null,9]]])"), Json::parse(R"({"values":[1,null,null,9]})"));
    EXPECT_TRUE(fails(R"({"values":[1]})", R"([["d",["values",1]]])"));
    EXPECT_EQ(applied(R"({"values":[1,2]})", R"([["d",["values",0]]])"), Json::parse(R"({"values":[2]})"));
}

TEST_F(DeltaApplierTest, ClampsSpliceRemovalPastTheEnd) {
    EXPECT_EQ(applied(R"({"values":[1,2]})", R"([["p",["values"],0,1000000000,[]]])"), Json::parse(R"({"values":[]})"));
}

TEST_F(DeltaApplierTest, AppendsAndTruncatesStringsInUtf16Units) {
    EXPECT_EQ(applied(R"({"value":"abc"})", R"([["t",["value"],2],["a",["value"],"xyz"]])"), Json::parse(R"({"value":"cxyz"})"));
    EXPECT_EQ(applied(R"({"value":"a😀z"})", R"([["t",["value"],3]])"), Json::parse(R"({"value":"z"})"));
    EXPECT_EQ(applied(R"({"value":"abc"})", R"([["t",["value"],10]])"), Json::parse(R"({"value":""})"));
    EXPECT_TRUE(fails(R"({"value":1})", R"([["a",["missing"],"x"]])"));
    EXPECT_TRUE(fails(R"({"value":"abc"})", R"([["t",["value"],-1]])"));
}

TEST_F(DeltaApplierTest, DeletesObjectKeysAndAllowsReservedNamesInsideValues) {
    EXPECT_EQ(applied(R"({"a":1,"b":2})", R"([["d",["a"]]])"), Json::parse(R"({"b":2})"));
    const Json out = applied("{}", R"([["s",["value"],{"__proto__":{"z":1}}]])");
    EXPECT_TRUE(out["value"].contains("__proto__"));
}

TEST_F(DeltaApplierTest, AppliesLargeSplicePayloads) {
    Json items = Json::array();
    for (int i = 0; i < 300000; ++i) {
        items.push_back(nullptr);
    }
    const auto result = m_applier.apply(Json{{"values", Json::array()}}, Json::array({Json::array({"p", Json::array({"values"}), 0, 0, items})}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["values"].size(), 300000U);
}

TEST_F(DeltaApplierTest, NumericPathSegmentsAddressObjectKeys) {
    EXPECT_EQ(applied(R"({"1":"a"})", R"([["s",["1"],"b"]])"), Json::parse(R"({"1":"b"})"));
    EXPECT_EQ(applied(R"({"1":{"x":1}})", R"([["s",[1,"x"],2]])"), Json::parse(R"({"1":{"x":2}})"));
}

TEST_F(DeltaApplierTest, AppliesLikeTheTypeScriptImplementation) {
    std::ifstream file("src/testing/ts_golden/ts_delta_golden.json");
    ASSERT_TRUE(file.good());
    const Json golden = Json::parse(file);
    int accepted = 0;
    for (const Json& vector : golden.at("apply")) {
        const std::string name = vector.at("name").get<std::string>();
        const auto result = m_applier.apply(vector.at("target"), vector.at("ops"));
        ASSERT_EQ(result.has_value(), vector.at("accepted").get<bool>()) << name << (result ? "" : " " + result.error().message);
        if (result) {
            EXPECT_EQ(*result, vector.at("result")) << name << " got=" << result->dump();
            ++accepted;
        }
    }
    EXPECT_GT(accepted, 200);
}
