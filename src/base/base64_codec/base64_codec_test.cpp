#include <gtest/gtest.h>

import std;
import pi.base.base64_codec;

class Base64CodecTest : public testing::Test {
protected:
    Base64Codec m_codec;
};

TEST_F(Base64CodecTest, Rfc4648Vectors) {
    EXPECT_EQ(m_codec.encode(""), "");
    EXPECT_EQ(m_codec.encode("f"), "Zg==");
    EXPECT_EQ(m_codec.encode("fo"), "Zm8=");
    EXPECT_EQ(m_codec.encode("foo"), "Zm9v");
    EXPECT_EQ(m_codec.encode("foobar"), "Zm9vYmFy");
}

TEST_F(Base64CodecTest, UrlAlphabetIsUnpadded) {
    EXPECT_EQ(m_codec.encodeUrl("\xfb\xff"), "-_8");
    EXPECT_EQ(m_codec.encode("\xfb\xff"), "+/8=");
}

TEST_F(Base64CodecTest, DecodesBothAlphabetsWithOrWithoutPadding) {
    EXPECT_EQ(m_codec.decode("Zm9vYmFy"), std::optional<std::string>("foobar"));
    EXPECT_EQ(m_codec.decode("Zg=="), std::optional<std::string>("f"));
    EXPECT_EQ(m_codec.decode("Zg"), std::optional<std::string>("f"));
    EXPECT_EQ(m_codec.decode("-_8"), std::optional<std::string>("\xfb\xff"));
}

TEST_F(Base64CodecTest, RejectsInvalidInput) {
    EXPECT_EQ(m_codec.decode("Z"), std::nullopt);
    EXPECT_EQ(m_codec.decode("Zm9v!"), std::nullopt);
}

TEST_F(Base64CodecTest, BinaryRoundTrip) {
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes.push_back(static_cast<char>(i));
    }
    EXPECT_EQ(m_codec.decode(m_codec.encode(bytes)), std::optional<std::string>(bytes));
    EXPECT_EQ(m_codec.decode(m_codec.encodeUrl(bytes)), std::optional<std::string>(bytes));
}
