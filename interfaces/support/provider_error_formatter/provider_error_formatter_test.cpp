#include <gtest/gtest.h>

import std;
import pi.support.provider_error_formatter;

TEST(ProviderErrorFormatterTest, FormatsStatusAndBody) {
    ProviderErrorFormatter formatter;
    HttpResponse response;
    response.status = 429;
    response.body = "  slow down \n";
    EXPECT_EQ(formatter.formatHttp(response), "429: slow down");
    EXPECT_EQ(formatter.formatHttp(response, "Mistral"), "Mistral (429): slow down");
}

TEST(ProviderErrorFormatterTest, EmptyBody) {
    ProviderErrorFormatter formatter;
    HttpResponse response;
    response.status = 403;
    EXPECT_EQ(formatter.formatHttp(response), "403 status code (no body)");
}

TEST(ProviderErrorFormatterTest, TruncatesLongBodies) {
    ProviderErrorFormatter formatter;
    HttpResponse response;
    response.status = 500;
    response.body = std::string(5000, 'x');
    const std::string out = formatter.formatHttp(response);
    EXPECT_NE(out.find("... [truncated 1000 chars]"), std::string::npos);
}

TEST(ProviderErrorFormatterTest, TruncateKeepsUtf8Intact) {
    ProviderErrorFormatter formatter;
    // 2-byte characters: cutting at byte 3 would split one.
    const std::string text = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    const std::string out = formatter.truncate(text, 3);
    EXPECT_EQ(out.rfind("\xC3\xA9... [truncated", 0), 0U);
}

TEST(ProviderErrorFormatterTest, ExtractsNestedMessage) {
    ProviderErrorFormatter formatter;
    EXPECT_EQ(formatter.extractMessage(R"({"error":{"message":"bad"}})"), "bad");
    EXPECT_EQ(formatter.extractMessage(R"({"error":"oops"})"), "oops");
    EXPECT_EQ(formatter.extractMessage(R"({"message":"m"})"), "m");
    EXPECT_EQ(formatter.extractMessage("not json"), "");
}
