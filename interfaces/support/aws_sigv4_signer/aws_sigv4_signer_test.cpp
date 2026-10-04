#include <gtest/gtest.h>

import std;
import pi.base.boring_crypto;
import pi.support.aws_sigv4_signer;

class AwsSigv4SignerTest : public testing::Test {
protected:
    std::optional<std::string> header(const HttpHeaders& headers, const std::string& name) {
        for (const auto& entry : headers) {
            if (entry.first == name) {
                return entry.second;
            }
        }
        return std::nullopt;
    }

    BoringCrypto m_crypto;
    AwsSigv4Signer m_signer{m_crypto};
};

TEST_F(AwsSigv4SignerTest, MatchesTheAwsGetVanillaTestVector) {
    Sigv4Request request;
    request.method = "GET";
    request.host = "example.amazonaws.com";
    request.path = "/";
    request.region = "us-east-1";
    request.service = "service";
    request.credentials = AwsCredentials{"AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", std::nullopt};
    // 2015-08-30T12:36:00Z
    const HttpHeaders added = m_signer.sign(request, 1440938160000);
    EXPECT_EQ(header(added, "x-amz-date"), "20150830T123600Z");
    EXPECT_EQ(header(added, "Authorization"),
              "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
              "SignedHeaders=host;x-amz-date, "
              "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
}

TEST_F(AwsSigv4SignerTest, SessionTokensAreSignedAndReturned) {
    Sigv4Request request;
    request.host = "bedrock-runtime.us-east-1.amazonaws.com";
    request.path = "/model/anthropic.claude-v2%3A1/converse-stream";
    request.headers = {{"Content-Type", "application/json"}};
    request.body = "{}";
    request.region = "us-east-1";
    request.service = "bedrock";
    request.credentials = AwsCredentials{"AKID", "secret", std::string("token")};
    const HttpHeaders added = m_signer.sign(request, 1440938160000);
    EXPECT_EQ(header(added, "x-amz-security-token"), "token");
    const std::string auth = *header(added, "Authorization");
    EXPECT_NE(auth.find("SignedHeaders=content-type;host;x-amz-date;x-amz-security-token"), std::string::npos);
    EXPECT_NE(auth.find("Credential=AKID/20150830/us-east-1/bedrock/aws4_request"), std::string::npos);
}

TEST_F(AwsSigv4SignerTest, SignaturesChangeWithTheBodyAndAreDeterministic) {
    Sigv4Request request;
    request.host = "h.example.com";
    request.region = "r";
    request.service = "s";
    request.credentials = AwsCredentials{"AKID", "secret", std::nullopt};
    const auto first = header(m_signer.sign(request, 1000), "Authorization");
    EXPECT_EQ(first, header(m_signer.sign(request, 1000), "Authorization"));
    request.body = "x";
    EXPECT_NE(first, header(m_signer.sign(request, 1000), "Authorization"));
}

TEST_F(AwsSigv4SignerTest, EncodeKeepsUnreservedCharacters) {
    EXPECT_EQ(m_signer.encode("a b/c:d~e", false), "a%20b%2Fc%3Ad~e");
    EXPECT_EQ(m_signer.encode("a b/c", true), "a%20b/c");
}
