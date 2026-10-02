#include "src/base/boring_crypto/boring_crypto.h"

#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

std::string BoringCrypto::sha256(std::string_view data) const {
    std::string out(SHA256_DIGEST_LENGTH, '\0');
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           reinterpret_cast<unsigned char*>(out.data()));
    return out;
}

std::string BoringCrypto::hmacSha256(std::string_view key, std::string_view data) const {
    std::string out(EVP_MAX_MD_SIZE, '\0');
    unsigned int length = 0;
    HMAC(EVP_sha256(), key.data(), key.size(), reinterpret_cast<const unsigned char*>(data.data()),
         data.size(), reinterpret_cast<unsigned char*>(out.data()), &length);
    out.resize(length);
    return out;
}

std::string BoringCrypto::randomBytes(std::size_t count) const {
    std::string out(count, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), count);
    return out;
}
