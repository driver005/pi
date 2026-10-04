export module pi.support.utf8_validator;

import std;

/** Strict UTF-8 check: no overlong forms, no surrogates, nothing above U+10FFFF, no truncation. */
export class Utf8Validator {
public:
    bool valid(std::string_view text) const {
        std::size_t i = 0;
        while (i < text.size()) {
            const auto lead = static_cast<unsigned char>(text[i]);
            if (lead < 0x80) {
                ++i;
            } else if (lead >= 0xC2 && lead <= 0xDF) {
                if (!continuation(text, i + 1)) {
                    return false;
                }
                i += 2;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                if (!continuation(text, i + 1) || !continuation(text, i + 2)) {
                    return false;
                }
                const auto second = static_cast<unsigned char>(text[i + 1]);
                if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F)) {
                    return false;
                }
                i += 3;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                if (!continuation(text, i + 1) || !continuation(text, i + 2) || !continuation(text, i + 3)) {
                    return false;
                }
                const auto second = static_cast<unsigned char>(text[i + 1]);
                if ((lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F)) {
                    return false;
                }
                i += 4;
            } else {
                return false;
            }
        }
        return true;
    }

private:
    bool continuation(std::string_view text, std::size_t index) const {
        return index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80;
    }
};
