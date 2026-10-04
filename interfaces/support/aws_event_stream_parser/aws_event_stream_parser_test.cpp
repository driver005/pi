#include <gtest/gtest.h>

import std;
import pi.support.aws_event_stream_parser;

class AwsEventStreamParserTest : public testing::Test {
protected:
    void putUint32(std::string& out, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xFF));
        }
    }

    std::string stringHeader(const std::string& name, const std::string& value) {
        std::string out;
        out.push_back(static_cast<char>(name.size()));
        out += name;
        out.push_back(7);
        out.push_back(static_cast<char>(value.size() >> 8));
        out.push_back(static_cast<char>(value.size() & 0xFF));
        out += value;
        return out;
    }

    std::string frame(const std::string& headers, const std::string& payload) {
        const std::uint32_t total = static_cast<std::uint32_t>(16 + headers.size() + payload.size());
        std::string out;
        putUint32(out, total);
        putUint32(out, static_cast<std::uint32_t>(headers.size()));
        putUint32(out, m_parser.crc32(out));
        out += headers;
        out += payload;
        putUint32(out, m_parser.crc32(out));
        return out;
    }

    AwsEventStreamParser m_parser;
};

TEST_F(AwsEventStreamParserTest, Crc32KnownVector) {
    EXPECT_EQ(m_parser.crc32("123456789"), 0xCBF43926U);
}

TEST_F(AwsEventStreamParserTest, DecodesAMessageWithStringAndTypedHeaders) {
    std::string headers = stringHeader(":event-type", "contentBlockDelta") + stringHeader(":message-type", "event");
    headers.push_back(1);
    headers += "x";
    headers.push_back(0);  // boolean true, no value
    headers.push_back(1);
    headers += "n";
    headers.push_back(4);  // int32
    headers += std::string("\0\0\0\x07", 4);
    const auto messages = m_parser.feed(frame(headers, R"({"a":1})"));
    ASSERT_TRUE(messages.has_value());
    ASSERT_EQ(messages->size(), 1U);
    EXPECT_EQ((*messages)[0].headers.at(":event-type"), "contentBlockDelta");
    EXPECT_EQ((*messages)[0].headers.at(":message-type"), "event");
    EXPECT_FALSE((*messages)[0].headers.contains("x"));
    EXPECT_EQ((*messages)[0].payload, R"({"a":1})");
}

TEST_F(AwsEventStreamParserTest, ReassemblesMessagesSplitAcrossChunks) {
    const std::string first = frame(stringHeader(":event-type", "a"), "one");
    const std::string second = frame(stringHeader(":event-type", "b"), "two");
    const std::string all = first + second;
    std::vector<AwsEventStreamMessage> got;
    for (std::size_t i = 0; i < all.size(); i += 5) {
        auto part = m_parser.feed(std::string_view(all).substr(i, 5));
        ASSERT_TRUE(part.has_value());
        for (auto& message : *part) {
            got.push_back(std::move(message));
        }
    }
    ASSERT_EQ(got.size(), 2U);
    EXPECT_EQ(got[0].payload, "one");
    EXPECT_EQ(got[1].payload, "two");
    EXPECT_TRUE(m_parser.finish().has_value());
}

TEST_F(AwsEventStreamParserTest, DetectsCorruptionAndTruncation) {
    std::string bad = frame(stringHeader(":event-type", "a"), "payload");
    bad[bad.size() - 6] = static_cast<char>(bad[bad.size() - 6] ^ 0x01);
    EXPECT_FALSE(m_parser.feed(bad).has_value());
    EXPECT_FALSE(m_parser.feed("more").has_value());

    AwsEventStreamParser prelude;
    std::string badPrelude = frame("", "x");
    badPrelude[9] = static_cast<char>(badPrelude[9] ^ 0x01);
    EXPECT_FALSE(prelude.feed(badPrelude).has_value());

    AwsEventStreamParser truncated;
    const std::string partial = frame("", "payload").substr(0, 20);
    ASSERT_TRUE(truncated.feed(partial).has_value());
    EXPECT_FALSE(truncated.finish().has_value());
}

TEST_F(AwsEventStreamParserTest, RejectsAbsurdLengths) {
    std::string huge;
    putUint32(huge, 0x7FFFFFFF);
    putUint32(huge, 0);
    putUint32(huge, 0);
    EXPECT_FALSE(m_parser.feed(huge).has_value());
}
