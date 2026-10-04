#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.session_entry_codec;

class SessionEntryCodecTest : public testing::Test {
protected:
    SessionEntryCodec m_codec;
};

TEST_F(SessionEntryCodecTest, ParsesLinesSkippingMalformedAndBlank) {
    const auto lines = m_codec.parseLines("{\"a\":1}\n\nnot json\n  \n{\"b\":2}");
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[1]["b"], 2);
}

TEST_F(SessionEntryCodecTest, HeaderFields) {
    const auto header = m_codec.headerFromJson(Json::parse(
        R"({"type":"session","version":3,"id":"s1","timestamp":"2025-01-01T00:00:00.000Z","cwd":"/w","parentSession":"/p.jsonl","extra":1})"));
    EXPECT_EQ(header.version, 3);
    EXPECT_EQ(header.id, "s1");
    EXPECT_EQ(header.cwd, "/w");
    EXPECT_EQ(header.parentSession, "/p.jsonl");
    EXPECT_EQ(header.body["extra"], 1);
    EXPECT_EQ(m_codec.headerFromJson(Json::parse(R"({"type":"session","id":"x"})")).version, 1);
}

TEST_F(SessionEntryCodecTest, EntryFields) {
    const auto entry = m_codec.entryFromJson(Json::parse(
        R"({"type":"message","id":"abcd1234","parentId":null,"timestamp":"t","message":{"role":"user"}})"));
    EXPECT_EQ(entry.type, "message");
    EXPECT_EQ(entry.id, "abcd1234");
    EXPECT_FALSE(entry.parentId.has_value());
    EXPECT_EQ(entry.body["message"]["role"], "user");
    const auto child = m_codec.entryFromJson(Json::parse(R"({"type":"x","id":"b","parentId":"abcd1234","timestamp":"t"})"));
    EXPECT_EQ(child.parentId, "abcd1234");
}

TEST_F(SessionEntryCodecTest, LineIsCompactWithNewline) {
    EXPECT_EQ(m_codec.line(Json::parse(R"({"a": [1, 2], "b": "é"})")), "{\"a\":[1,2],\"b\":\"é\"}\n");
    EXPECT_TRUE(m_codec.isHeader(Json::parse(R"({"type":"session"})")));
    EXPECT_FALSE(m_codec.isHeader(Json::parse(R"({"type":"message"})")));
}
