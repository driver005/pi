module;

#include <cstddef>

export module pi.platform.i_crypto;

import std;
export import pi.types.result;

/** Hashing and randomness. Binary results are returned as raw byte strings. */
export class ICrypto {
public:
    virtual ~ICrypto() = default;

    virtual std::string sha256(std::string_view data) const = 0;
    virtual std::string hmacSha256(std::string_view key, std::string_view data) const = 0;
    virtual std::string randomBytes(std::size_t count) const = 0;
    /** RSASSA-PKCS1-v1_5 with SHA-256 (RS256) over data with a PEM private key; raw signature bytes. */
    virtual Result<std::string> rsaSha256Sign(std::string_view pemPrivateKey, std::string_view data) const = 0;
};
