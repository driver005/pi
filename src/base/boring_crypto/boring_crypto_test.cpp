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
