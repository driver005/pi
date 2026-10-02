module;

#include <cstddef>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

export module pi.base.boring_crypto;

import std;
export import pi.platform.i_crypto;

/** ICrypto backed by BoringSSL (the TLS library curl already links). */
export class BoringCrypto : public ICrypto {
public:
    std::string sha256(std::string_view data) const override;
    std::string hmacSha256(std::string_view key, std::string_view data) const override;
    std::string randomBytes(std::size_t count) const override;
    Result<std::string> rsaSha256Sign(std::string_view pemPrivateKey, std::string_view data) const override;
};

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

Result<std::string> BoringCrypto::rsaSha256Sign(std::string_view pemPrivateKey, std::string_view data) const {
    BIO* bio = BIO_new_mem_buf(pemPrivateKey.data(), static_cast<int>(pemPrivateKey.size()));
    EVP_PKEY* key = bio != nullptr ? PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr) : nullptr;
    if (bio != nullptr) {
        BIO_free(bio);
    }
    if (key == nullptr) {
        return std::unexpected(Error{"invalid_key", "Could not read the PEM private key"});
    }
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    std::size_t length = 0;
    std::string signature;
    bool ok = context != nullptr && EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
              EVP_DigestSign(context, nullptr, &length, reinterpret_cast<const unsigned char*>(data.data()),
                             data.size()) == 1;
    if (ok) {
        signature.resize(length);
        ok = EVP_DigestSign(context, reinterpret_cast<unsigned char*>(signature.data()), &length,
                            reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 1;
        signature.resize(length);
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!ok) {
        return std::unexpected(Error{"sign_failed", "RSA signing failed"});
    }
    return signature;
}
