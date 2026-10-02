#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "interfaces/platform/i_base64_codec/i_base64_codec.h"

class Base64Codec : public IBase64Codec {
public:
    std::string encode(std::string_view bytes) const override;
    std::string encodeUrl(std::string_view bytes) const override;
    std::optional<std::string> decode(std::string_view text) const override;

private:
    std::string encodeWith(std::string_view bytes, const char* alphabet, bool pad) const;
    int valueOf(char c) const;
};
