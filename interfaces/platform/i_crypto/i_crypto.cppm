module;

#include <cstddef>

export module pi.platform.i_crypto;

import std;

/** Hashing and randomness. Binary results are returned as raw byte strings. */
export class ICrypto {
public:
    virtual ~ICrypto() = default;

    virtual std::string sha256(std::string_view data) const = 0;
    virtual std::string hmacSha256(std::string_view key, std::string_view data) const = 0;
    virtual std::string randomBytes(std::size_t count) const = 0;
};
