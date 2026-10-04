#include <gtest/gtest.h>

import std;
import pi.support.ini_parser;

TEST(IniParserTest, ParsesSectionsKeysAndComments) {
    IniParser parser;
    const auto sections = parser.parse(
        "ignored = 1\n"
        "# comment\n"
        "[default]\n"
        "aws_access_key_id = AKID\n"
        "aws_secret_access_key=secret value \n"
        "; another\n"
        "\n"
        "[ profile dev ]\n"
        "region = eu-west-1\r\n"
        "bad line without equals\n");
    ASSERT_EQ(sections.size(), 2U);
    EXPECT_EQ(sections.at("default").at("aws_access_key_id"), "AKID");
    EXPECT_EQ(sections.at("default").at("aws_secret_access_key"), "secret value");
    EXPECT_EQ(sections.at("profile dev").at("region"), "eu-west-1");
    EXPECT_FALSE(sections.contains(""));
}

TEST(IniParserTest, EmptyInput) {
    EXPECT_TRUE(IniParser().parse("").empty());
}
