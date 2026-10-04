module;

#include <cstdint>

export module pi.support.short_hash;

import std;

/** Fast deterministic hash that shortens long ids. Port of utils/hash.ts (same output). */
export class ShortHash {
public:
    /** Base-36 digest of the string hashed over its UTF-16 code units, like the TS version. */
    std::string of(const std::string& text) const {
        std::uint32_t h1 = 0xdeadbeef;
        std::uint32_t h2 = 0x41c6ce57;
        for (const std::uint16_t unit : toUtf16(text)) {
            h1 = (h1 ^ unit) * 2654435761U;
            h2 = (h2 ^ unit) * 1597334677U;
        }
        h1 = ((h1 ^ (h1 >> 16)) * 2246822507U) ^ ((h2 ^ (h2 >> 13)) * 3266489909U);
        h2 = ((h2 ^ (h2 >> 16)) * 2246822507U) ^ ((h1 ^ (h1 >> 13)) * 3266489909U);
        return toBase36(h2) + toBase36(h1);
    }

private:
    std::vector<std::uint16_t> toUtf16(const std::string& text) const {
        std::vector<std::uint16_t> units;
        std::size_t i = 0;
        while (i < text.size()) {
            const auto lead = static_cast<unsigned char>(text[i]);
            std::uint32_t code = lead;
            std::size_t length = 1;
            if (lead >= 0xF0) {
                code = lead & 0x07;
                length = 4;
            } else if (lead >= 0xE0) {
                code = lead & 0x0F;
                length = 3;
            } else if (lead >= 0xC0) {
                code = lead & 0x1F;
                length = 2;
            }
            for (std::size_t k = 1; k < length && i + k < text.size(); ++k) {
                code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            }
            i += length;
            if (code >= 0x10000) {
                code -= 0x10000;
                units.push_back(static_cast<std::uint16_t>(0xD800 + (code >> 10)));
                units.push_back(static_cast<std::uint16_t>(0xDC00 + (code & 0x3FF)));
            } else {
                units.push_back(static_cast<std::uint16_t>(code));
            }
        }
        return units;
    }

    std::string toBase36(std::uint32_t value) const {
        if (value == 0) {
            return "0";
        }
        std::string out;
        while (value > 0) {
            const std::uint32_t digit = value % 36;
            out.push_back(static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10));
            value /= 36;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
};
