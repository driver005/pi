#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "interfaces/platform/i_crypto/i_crypto.h"

/** ICrypto backed by BoringSSL (the TLS library curl already links). */
class BoringCrypto : public ICrypto {
public:
    std::string sha256(std::string_view data) const override;
    std::string hmacSha256(std::string_view key, std::string_view data) const override;
    std::string randomBytes(std::size_t count) const override;
};
