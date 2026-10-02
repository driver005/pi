export module pi.base.base64_codec;

import std;
export import pi.platform.i_base64_codec;

export class Base64Codec : public IBase64Codec {
public:
    std::string encode(std::string_view bytes) const override;
    std::string encodeUrl(std::string_view bytes) const override;
    std::optional<std::string> decode(std::string_view text) const override;

private:
    std::string encodeWith(std::string_view bytes, const char* alphabet, bool pad) const;
    int valueOf(char c) const;
};

std::string Base64Codec::encodeWith(std::string_view bytes, const char* alphabet, bool pad) const {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const unsigned int n = (static_cast<unsigned char>(bytes[i]) << 16) |
                               (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                               static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
        i += 3;
    }
    const std::size_t rest = bytes.size() - i;
    if (rest > 0) {
        unsigned int n = static_cast<unsigned char>(bytes[i]) << 16;
        n |= rest == 2 ? static_cast<unsigned char>(bytes[i + 1]) << 8 : 0;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        if (rest == 2) {
            out.push_back(alphabet[(n >> 6) & 63]);
        }
        if (pad) {
            out.append(rest == 2 ? "=" : "==");
        }
    }
    return out;
}

std::string Base64Codec::encode(std::string_view bytes) const {
    return encodeWith(bytes, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

std::string Base64Codec::encodeUrl(std::string_view bytes) const {
    return encodeWith(bytes, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

int Base64Codec::valueOf(char c) const {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

std::optional<std::string> Base64Codec::decode(std::string_view text) const {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string out;
    unsigned int buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const int value = valueOf(c);
        if (value < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<unsigned int>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}
