#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.partial_json_parser;

class PartialJsonParserTest : public testing::Test {
protected:
    PartialJsonParser m_parser;

    std::string stream(const std::string& text) {
        return m_parser.parseStreaming(text).dump();
    }
};

TEST_F(PartialJsonParserTest, EmptyInputGivesEmptyObject) {
    EXPECT_EQ(stream(""), "{}");
    EXPECT_EQ(stream("  \n"), "{}");
}

TEST_F(PartialJsonParserTest, CompleteJsonIsReturnedAsIs) {
    EXPECT_EQ(stream("{\"a\":1,\"b\":[true,null]}"), "{\"a\":1,\"b\":[true,null]}");
}

TEST_F(PartialJsonParserTest, KeepsPartialString) {
    EXPECT_EQ(stream("{\"path\":\"/tmp/fo"), "{\"path\":\"/tmp/fo\"}");
}

TEST_F(PartialJsonParserTest, DropsDanglingKey) {
    EXPECT_EQ(stream("{\"a\":1,\"b\":"), "{\"a\":1}");
    EXPECT_EQ(stream("{\"a\":1,\"b"), "{\"a\":1}");
    EXPECT_EQ(stream("{\"a\":1,"), "{\"a\":1}");
}

TEST_F(PartialJsonParserTest, CompletesPartialLiteralsAndNested) {
    EXPECT_EQ(stream("[1,2,{\"x\":tr"), "[1,2,{\"x\":true}]");
    EXPECT_EQ(stream("{\"a\":{\"b\":[1,2"), "{\"a\":{\"b\":[1,2]}}");
}

TEST_F(PartialJsonParserTest, TruncatesDanglingNumberSuffix) {
    EXPECT_EQ(stream("{\"n\":12."), "{\"n\":12}");
    EXPECT_EQ(stream("{\"n\":-"), "{}");
}

TEST_F(PartialJsonParserTest, HandlesTruncatedEscapes) {
    EXPECT_EQ(stream("{\"s\":\"a\\"), "{\"s\":\"a\"}");
    EXPECT_EQ(stream("{\"s\":\"a\\u00"), "{\"s\":\"a\"}");
    EXPECT_EQ(stream("{\"s\":\"\\u00e9"), "{\"s\":\"\xc3\xa9\"}");
}

TEST_F(PartialJsonParserTest, RepairsRawNewlinesAndBadEscapes) {
    EXPECT_EQ(stream("{\"a\":\"x\ny\"}"), "{\"a\":\"x\\ny\"}");
    EXPECT_EQ(stream("{\"p\":\"C:\\dir\"}"), "{\"p\":\"C:\\\\dir\"}");
}

TEST_F(PartialJsonParserTest, ParseWithRepairFailsOnGarbage) {
    EXPECT_FALSE(m_parser.parseWithRepair("{not json").has_value());
}

TEST_F(PartialJsonParserTest, PreservesKeyInsertionOrder) {
    EXPECT_EQ(stream("{\"z\":1,\"a\":2"), "{\"z\":1,\"a\":2}");
}

TEST_F(PartialJsonParserTest, DeepNestingDoesNotOverflow) {
    const std::string deep(5000, '[');
    EXPECT_EQ(stream(deep), "{}");
}
