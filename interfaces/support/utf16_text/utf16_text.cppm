export module pi.support.utf16_text;

import std;

/**
 * UTF-16 code unit arithmetic over UTF-8 text. Delta string operations count in JavaScript string
 * units (UTF-16 code units), so the C++ side translates between those and byte offsets. The text
 * must be valid UTF-8.
 */
export class Utf16Text {
public:
    /** Length of the text in UTF-16 code units. */
    std::size_t length(std::string_view text) const;

    /**
     * The byte offset where the text's first `units` UTF-16 code units end; nullopt when `units`
     * exceeds the length or falls inside a surrogate pair.
     */
    std::optional<std::size_t> byteOffset(std::string_view text, std::size_t units) const;

    /** Whether `index` is the start of a code point (or the end of the text). */
    bool boundary(std::string_view text, std::size_t index) const;

private:
    std::size_t units(unsigned char lead) const;
    std::size_t width(unsigned char lead) const;
};

std::size_t Utf16Text::width(unsigned char lead) const {
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xF0) {
        return 4;
    }
    return lead >= 0xE0 ? 3 : 2;
}

std::size_t Utf16Text::units(unsigned char lead) const {
    return lead >= 0xF0 ? 2 : 1;
}

bool Utf16Text::boundary(std::string_view text, std::size_t index) const {
    return index >= text.size() || (static_cast<unsigned char>(text[index]) & 0xC0) != 0x80;
}

std::size_t Utf16Text::length(std::string_view text) const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); i += width(static_cast<unsigned char>(text[i]))) {
        count += units(static_cast<unsigned char>(text[i]));
    }
    return count;
}

std::optional<std::size_t> Utf16Text::byteOffset(std::string_view text, std::size_t target) const {
    std::size_t count = 0;
    std::size_t i = 0;
    while (count < target && i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        count += units(lead);
        i += width(lead);
    }
    if (count != target) {
        return std::nullopt;
    }
    return std::min(i, text.size());
}
