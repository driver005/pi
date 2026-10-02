#pragma once

#include <cstddef>
#include <string>
#include <string_view>

/** Hashing and randomness. Binary results are returned as raw byte strings. */
class ICrypto {
public:
    virtual ~ICrypto() = default;

    virtual std::string sha256(std::string_view data) const = 0;
    virtual std::string hmacSha256(std::string_view key, std::string_view data) const = 0;
    virtual std::string randomBytes(std::size_t count) const = 0;
};
