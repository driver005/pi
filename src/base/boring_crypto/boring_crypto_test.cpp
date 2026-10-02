#include <gtest/gtest.h>

import std;
import pi.base.boring_crypto;

class BoringCryptoTest : public testing::Test {
protected:
    std::string hex(const std::string& bytes) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (const unsigned char c : bytes) {
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0xF]);
        }
        return out;
    }

    BoringCrypto m_crypto;
};

TEST_F(BoringCryptoTest, Sha256KnownVector) {
    EXPECT_EQ(hex(m_crypto.sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex(m_crypto.sha256("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(BoringCryptoTest, HmacSha256Rfc4231Vector) {
    EXPECT_EQ(hex(m_crypto.hmacSha256("Jefe", "what do ya want for nothing?")),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_F(BoringCryptoTest, RandomBytesHaveRequestedLengthAndDiffer) {
    const std::string a = m_crypto.randomBytes(32);
    const std::string b = m_crypto.randomBytes(32);
    EXPECT_EQ(a.size(), 32U);
    EXPECT_NE(a, b);
}

TEST_F(BoringCryptoTest, RsaSha256SignatureMatchesOpenSsl) {
    const std::string key = R"(-----BEGIN PRIVATE KEY-----
MIICeAIBADANBgkqhkiG9w0BAQEFAASCAmIwggJeAgEAAoGBANRLNx6mqBUz4oe5
LkkKXXSUbbd3LbU1dpIFW9I7NR8jWVBP9S2AYsg2VgTWaiEpZdxcFRagBZccb1my
klsArUtcK+RpBZ1jJtTSQFbwWd1kGNMgLJM5ZFOv6Xkgt8LBnUr06qxm2UCAGEvW
w7Gayy89YxMplwqY+vsTMsTuVFonAgMBAAECgYEAmB045paF04N06slOl/l8U19T
amVT9AbV6fU7AN15x9D9WyWfyTW4EjuU0SyNqStPmGDGn4qT1t4CD2R7qAdJI2sj
KLECQv69yD0XSOLqcioeOUMAlgB4/nOeve2wyb9qYiWB1vHm9t8//Pnv4e3pTyMU
DsZjdJUS4R0lW1jgY0kCQQDxY2nQ4Rk8gpAtVbYU9cJF5wsMb+5aVLZvMgRU5OeF
5kTQpfBPKzZpXi2cXVzUEtZQCnHRk7U2OpCbpPDITGl9AkEA4STzAe41LJL+A7HO
i6NnUoFr36G1NDK2O+V5sHghChgWBc4jke6BYPTO8kmGnDDvinQqueqZll62QSfe
8P6DcwJBANnyvecIZ0XYSR91xTp1j1yYOMSZB6ft1u7dRUX1jAm9GKMfQLPqu201
yI7nSVp+S5znYU8uQ67cABdYPMNNIu0CQFoP1cWn7E1wX3xK3DyvmN1AOE60+S9w
OcWr/gnBhDXtfKHF3CS8K7UFOONi1h4U1T2lSpIkblvgdzeJI31z0lECQQDhvarD
YUlbP5V2vpZ6mdnBPeD0iTUJpZBaq/RoxSKuuFbTJVnzEfs6XBQkO8EwmgSJIQfd
2/lrNVTIFy3LJDqm
-----END PRIVATE KEY-----
)";
    const auto signature = m_crypto.rsaSha256Sign(key, "hello world");
    ASSERT_TRUE(signature.has_value());
    EXPECT_EQ(hex(*signature),
              "2f0421db5f7e43a86893194434c2a73e1f276e3691f7a4cbcd2a02c0b7f0d82e071884ed5bb7379152e926703d077a72abfb66aae678244f32a657857488432abcb1adc3c9be5683a2ee7a36e39263e0a83e4daddfe6a91aed2af5b9e523343227c9e21aa8d97eb35e78a39b6c24ce4f1555a289656312f2df37ad7910cf73ca");
}

TEST_F(BoringCryptoTest, RsaSignRejectsBadKeys) {
    EXPECT_FALSE(m_crypto.rsaSha256Sign("not a key", "x").has_value());
}
