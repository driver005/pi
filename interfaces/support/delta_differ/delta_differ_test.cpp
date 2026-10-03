#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.delta_applier;
import pi.support.delta_differ;

class DeltaDifferTest : public testing::Test {
protected:
    Json j(const std::string& text) {
        return Json::parse(text);
    }

    /** JSON equality ignores object key order. */
    bool sameValue(const Json& left, const Json& right) {
        return nlohmann::json::parse(left.dump()) == nlohmann::json::parse(right.dump());
    }

    Json checked(const Json& before, const Json& after) {
        const Json ops = m_differ.diff(before, after);
        const auto result = m_applier.apply(before, ops);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message) << " ops=" << ops.dump();
        if (result) {
            EXPECT_TRUE(sameValue(*result, after)) << "ops=" << ops.dump() << " got=" << result->dump() << " want=" << after.dump();
        }
        return ops;
    }

    DeltaDiffer m_differ;
    DeltaApplier m_applier;
};

TEST_F(DeltaDifferTest, EmitsNothingForEqualValues) {
    EXPECT_EQ(checked(j(R"({"a":[1,{"b":2}]})"), j(R"({"a":[1,{"b":2}]})")), Json::array());
}

TEST_F(DeltaDifferTest, EmitsSetsAndDeletes) {
    EXPECT_EQ(checked(j(R"({"keep":1,"change":1,"remove":true})"), j(R"({"keep":1,"change":2,"add":3})")),
              j(R"([["s",["change"],2],["s",["add"],3],["d",["remove"]]])"));
}

TEST_F(DeltaDifferTest, EmitsStringAppendAndFrontTruncation) {
    EXPECT_EQ(checked(j(R"({"text":"hello"})"), j(R"({"text":"hello world"})")), j(R"([["a",["text"]," world"]])"));
    EXPECT_EQ(checked(j(R"({"text":"hello world"})"), j(R"({"text":"world"})")), j(R"([["t",["text"],6]])"));
    EXPECT_EQ(checked(j(R"({"text":"abcdefgh"})"), j(R"({"text":"defghxyz"})")), j(R"([["t",["text"],3],["a",["text"],"xyz"]])"));
    EXPECT_EQ(checked(j(R"({"text":"abc"})"), j(R"({"text":"xyz"})")), j(R"([["s",["text"],"xyz"]])"));
}

TEST_F(DeltaDifferTest, CountsTruncationInUtf16Units) {
    const Json before = j(R"({"text":"😀abc"})");
    const Json after = j(R"({"text":"abcdef"})");
    EXPECT_EQ(checked(before, after), j(R"([["t",["text"],2],["a",["text"],"def"]])"));
}

TEST_F(DeltaDifferTest, RepresentsArrayInsertionAndRemovalWithSplices) {
    EXPECT_EQ(checked(j(R"({"values":[{"id":"a"},{"id":"b"}]})"), j(R"({"values":[{"id":"a"},{"id":"c"},{"id":"b"}]})")),
              j(R"([["p",["values"],1,0,[{"id":"c"}]]])"));
    EXPECT_EQ(checked(j(R"({"values":[{"id":"a"},{"id":"b"},{"id":"c"}]})"), j(R"({"values":[{"id":"b"},{"id":"c"}]})")),
              j(R"([["p",["values"],0,1,[]]])"));
}

TEST_F(DeltaDifferTest, EmitsAPermutationForAPureReorderOfScalars) {
    EXPECT_EQ(checked(j(R"({"values":["a","b","c"]})"), j(R"({"values":["c","a","b"]})")), j(R"([["m",["values"],[2,0,1]]])"));
}

TEST_F(DeltaDifferTest, SplicesAReorderOfContainersBecauseTheyHaveNoIdentity) {
    EXPECT_EQ(checked(j(R"({"values":[{"id":"a"},{"id":"b"},{"id":"c"}]})"), j(R"({"values":[{"id":"c"},{"id":"a"},{"id":"b"}]})")),
              j(R"([["p",["values"],0,0,[{"id":"c"}]],["p",["values"],3,1,[]]])"));
}

TEST_F(DeltaDifferTest, KeepsLeafEditsNarrow) {
    EXPECT_EQ(checked(j(R"({"rows":[{"v":1},{"v":2},{"v":3}]})"), j(R"({"rows":[{"v":1},{"v":9},{"v":3}]})")),
              j(R"([["s",["rows",1,"v"],9]])"));
}

TEST_F(DeltaDifferTest, ReplacesTheRootWhenTypesDiffer) {
    EXPECT_EQ(checked(j("[1]"), j(R"({"a":1})")), j(R"([["r",{"a":1}]])"));
    EXPECT_EQ(checked(j("1"), j("2")), j(R"([["r",2]])"));
}

TEST_F(DeltaDifferTest, SetsObjectsWithReservedKeysWholesale) {
    EXPECT_EQ(checked(j(R"({"v":{"a":1}})"), j(R"({"v":{"a":1,"__proto__":{"x":1}}})")), j(R"([["s",["v"],{"a":1,"__proto__":{"x":1}}]])"));
}

TEST_F(DeltaDifferTest, BoundsWideChangesWithARootReplacement) {
    Json before = Json::object();
    Json after = Json::object();
    for (int i = 0; i < 5000; ++i) {
        before["k" + std::to_string(i)] = i;
        after["k" + std::to_string(i)] = i + 1;
    }
    const Json ops = checked(before, after);
    ASSERT_EQ(ops.size(), 1U);
    EXPECT_EQ(ops[0][0], "r");
}

TEST_F(DeltaDifferTest, HandlesRollingWindowsOfArrays) {
    Json before = Json::array();
    Json after = Json::array();
    for (int i = 0; i < 200; ++i) {
        before.push_back(i);
        after.push_back(i + 50);
    }
    checked(Json{{"rows", before}}, Json{{"rows", after}});
}

class Prng {
public:
    explicit Prng(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t next() {
        m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return m_state >> 33;
    }

    std::uint64_t below(std::uint64_t bound) {
        return next() % bound;
    }

private:
    std::uint64_t m_state;
};

TEST_F(DeltaDifferTest, RandomRevisionsRoundTrip) {
    Prng random(12345);
    const std::function<Json(int)> make = [&](int depth) -> Json {
        const auto kind = random.below(depth > 2 ? 4 : 7);
        switch (kind) {
        case 0:
            return nullptr;
        case 1:
            return static_cast<int>(random.below(5));
        case 2:
            return std::string(random.below(4), static_cast<char>('a' + random.below(3)));
        case 3:
            return random.below(2) == 0;
        case 4:
        case 5: {
            Json array = Json::array();
            for (auto n = random.below(5); n > 0; --n) {
                array.push_back(make(depth + 1));
            }
            return array;
        }
        default: {
            Json object = Json::object();
            for (auto n = random.below(4); n > 0; --n) {
                object[std::string(1, static_cast<char>('a' + random.below(4)))] = make(depth + 1);
            }
            return object;
        }
        }
    };
    for (int round = 0; round < 3000; ++round) {
        const Json before = make(0);
        const Json after = random.below(3) == 0 ? make(0) : [&]() {
            Json copy = before;
            if (copy.is_array() && !copy.empty()) {
                copy[random.below(copy.size())] = make(2);
                if (random.below(2) == 0) {
                    copy.push_back(make(2));
                }
            } else if (copy.is_object() && !copy.empty()) {
                copy[copy.begin().key()] = make(2);
            }
            return copy;
        }();
        checked(before, after);
    }
}

TEST_F(DeltaDifferTest, DiffsLikeTheTypeScriptImplementation) {
    std::ifstream file("src/testing/ts_golden/ts_delta_golden.json");
    ASSERT_TRUE(file.good());
    const Json golden = Json::parse(file);
    for (const Json& vector : golden.at("diff")) {
        EXPECT_EQ(m_differ.diff(vector.at("before"), vector.at("after")), vector.at("ops")) << vector.at("name").get<std::string>();
    }
}
