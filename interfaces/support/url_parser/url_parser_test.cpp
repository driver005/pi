#include <gtest/gtest.h>

import std;
import pi.support.url_parser;

class UrlParserTest : public testing::Test {
protected:
    std::string normalized(const std::string& text) {
        const auto result = m_parser.normalize(text);
        return result ? *result : "ERROR " + result.error().message;
    }

    UrlParser m_parser;
};

TEST_F(UrlParserTest, BareOriginsGetASlashAndHostsAreLowerCase) {
    EXPECT_EQ(normalized("https://Example.COM"), "https://example.com/");
    EXPECT_EQ(normalized("HTTP://localhost:8080"), "http://localhost:8080/");
}

TEST_F(UrlParserTest, DefaultPortsAreDropped) {
    EXPECT_EQ(normalized("https://example.com:443/mcp"), "https://example.com/mcp");
    EXPECT_EQ(normalized("http://example.com:80/mcp"), "http://example.com/mcp");
    EXPECT_EQ(normalized("https://example.com:8443/mcp"), "https://example.com:8443/mcp");
}

TEST_F(UrlParserTest, QueryAndFragmentSurvive) {
    EXPECT_EQ(normalized("https://example.com/a?x=1#frag"), "https://example.com/a?x=1#frag");
    const auto url = m_parser.parse("https://example.com/a?x=1#frag");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(m_parser.print(*url, false), "https://example.com/a?x=1");
    EXPECT_EQ(m_parser.origin(*url), "https://example.com");
}

TEST_F(UrlParserTest, Ipv6HostsKeepTheirBrackets) {
    EXPECT_EQ(normalized("http://[::1]:3000/cb"), "http://[::1]:3000/cb");
    const auto url = m_parser.parse("http://[::1]/");
    ASSERT_TRUE(url.has_value());
    EXPECT_TRUE(m_parser.loopback(*url));
}

TEST_F(UrlParserTest, RejectsWhatIsNotAnHttpUrl) {
    for (const char* bad : {"example.com", "ftp://example.com/", "https://", "https://user@example.com/", "https://example.com:99999/", "https://example.com:x/", "javascript:alert(1)", "https://[::1/"}) {
        EXPECT_FALSE(m_parser.parse(bad).has_value()) << bad;
    }
}

TEST_F(UrlParserTest, LoopbackHostsAreRecognized) {
    for (const char* ok : {"http://localhost/", "http://127.0.0.1:9/", "http://[::1]/"}) {
        EXPECT_TRUE(m_parser.loopback(*m_parser.parse(ok))) << ok;
    }
    EXPECT_FALSE(m_parser.loopback(*m_parser.parse("https://example.com/")));
}
