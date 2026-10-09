export module pi.support.theme_color_parser;

import std;
import pi.support.oklab_converter;
export import pi.types.json;
export import pi.types.result;
export import pi.types.theme_color;

/**
 * Theme color values: `#rgb`, `#rrggbb`, `oklch(L C H)` (L may be a percentage), `okhsl(H S L)`
 * (S and L may be percentages) or a 256-color palette index. Port of parseColor in
 * packages/tui/src/colors.ts and its conversions to sRGB and hex. Numbers are read with
 * `std::from_chars`, so a malformed or out-of-range number is an error, never an exception.
 */
export class ThemeColorParser {
public:
    Result<ThemeColor> parse(const Json& value) const {
        if (value.is_number_integer()) {
            return indexed(value.get<std::int64_t>());
        }
        if (!value.is_string()) {
            return std::unexpected(invalid(value.dump()));
        }
        const std::string text = value.get<std::string>();
        if (text.starts_with("#")) {
            return hex(text);
        }
        if (hasPrefix(text, "oklch(")) {
            return oklch(text);
        }
        if (hasPrefix(text, "okhsl(")) {
            return okhsl(text);
        }
        return std::unexpected(invalid(text));
    }

    /** Whether the text starts like an `oklch(` or `okhsl(` color (any case). */
    bool isOkColorFunction(const std::string& text) const {
        return hasPrefix(text, "oklch(") || hasPrefix(text, "okhsl(");
    }

    /** Whether the text starts like an `okhsl(` color (any case). */
    bool isOkhsl(const std::string& text) const {
        return hasPrefix(text, "okhsl(");
    }

    /** `#rrggbb` of an sRGB color (channels rounded half up). */
    std::string toHex(const RgbColor& color) const {
        std::string out = "#";
        for (const double channel : {color.r, color.g, color.b}) {
            out += std::format("{:02x}", static_cast<int>(std::floor(channel + 0.5)));
        }
        return out;
    }

private:
    Error invalid(const std::string& text) const {
        return Error{"invalid_color", "Invalid color value: " + text};
    }

    Result<ThemeColor> indexed(std::int64_t index) const {
        if (index < 0 || index > 255) {
            return std::unexpected(Error{
                "invalid_color",
                "ANSI color index must be an integer from 0 to 255: " + std::to_string(index)});
        }
        return withLightness(paletteColor(static_cast<int>(index)), index < 16);
    }

    Result<ThemeColor> hex(const std::string& text) const {
        const std::string digits = text.substr(1);
        const bool valid =
            (digits.size() == 3 || digits.size() == 6) &&
            std::ranges::all_of(digits, [](unsigned char c) { return std::isxdigit(c) != 0; });
        if (!valid) {
            return std::unexpected(invalid(text));
        }
        const std::string full = digits.size() == 3 ? std::string{digits[0], digits[0], digits[1],
                                                                  digits[1], digits[2], digits[2]}
                                                    : digits;
        const auto channel = [&](std::size_t at) {
            int value = 0;
            std::from_chars(full.data() + at, full.data() + at + 2, value, 16);
            return static_cast<double>(value);
        };
        return withLightness(RgbColor{channel(0), channel(2), channel(4)}, false);
    }

    /** `oklch(L[%] C H[deg])`. */
    Result<ThemeColor> oklch(const std::string& text) const {
        const std::vector<std::string> words = arguments(text);
        if (words.size() != 3) {
            return std::unexpected(invalid(text));
        }
        const auto lightness = number(words[0], "%", true);
        const auto chroma = number(words[1], "", false);
        const auto hue = number(words[2], "deg", false);
        if (!lightness || !chroma || !hue || *lightness < 0 || *lightness > 1 || *chroma < 0) {
            return std::unexpected(invalid(text));
        }
        ThemeColor color;
        color.rgb =
            m_oklab.oklchToRgb(*lightness, *chroma, std::fmod(std::fmod(*hue, 360) + 360, 360));
        color.lightness = *lightness;
        return color;
    }

    /** `okhsl(H[deg] S[%] L[%])`. */
    Result<ThemeColor> okhsl(const std::string& text) const {
        const std::vector<std::string> words = arguments(text);
        if (words.size() != 3) {
            return std::unexpected(invalid(text));
        }
        const auto hue = number(words[0], "deg", false);
        const auto saturation = number(words[1], "%", true);
        const auto lightness = number(words[2], "%", true);
        if (!hue || !saturation || !lightness || *saturation < 0 || *saturation > 1 ||
            *lightness < 0 || *lightness > 1) {
            return std::unexpected(invalid(text));
        }
        return withLightness(m_oklab.okhslToRgb(*hue, *saturation, *lightness), false);
    }

    bool hasPrefix(const std::string& text, std::string_view prefix) const {
        return text.size() >= prefix.size() &&
               std::ranges::equal(
                   text.substr(0, prefix.size()), prefix,
                   [](unsigned char a, unsigned char b) { return std::tolower(a) == b; });
    }

    /** The whitespace-separated words between the parentheses; empty when the text does not end
     * with `)`. */
    std::vector<std::string> arguments(const std::string& text) const {
        std::vector<std::string> words;
        if (!text.ends_with(")")) {
            return words;
        }
        const std::size_t open = text.find('(');
        std::istringstream stream(text.substr(open + 1, text.size() - open - 2));
        for (std::string word; stream >> word;) {
            words.push_back(word);
        }
        return words;
    }

    /**
     * A decimal number (`[+-]?digits[.digits][e[+-]digits]`) followed by nothing or `suffix` (any
     * case); with `percent` a `%` suffix divides the number by 100. Nothing for anything else,
     * including out-of-range numbers, `inf` and `nan`.
     */
    std::optional<double> number(const std::string& word, std::string_view suffix,
                                 bool percent) const {
        const std::size_t start = word.starts_with("+") ? 1 : 0;
        if (!startsNumber(word, start)) {
            return std::nullopt;
        }
        double value = 0;
        const char* end = word.data() + word.size();
        const auto parsed = std::from_chars(word.data() + start, end, value);
        if (parsed.ec != std::errc() || !std::isfinite(value)) {
            return std::nullopt;
        }
        const std::string rest(parsed.ptr, end);
        if (rest.empty()) {
            return value;
        }
        if (suffix.empty() || rest.size() != suffix.size() || !hasPrefix(rest, suffix)) {
            return std::nullopt;
        }
        return percent ? value / 100 : value;
    }

    /** Whether a digit, or a `.` or `-` followed by one, is at `at`: `from_chars` alone would also
     * take `inf` and `nan`. */
    bool startsNumber(const std::string& word, std::size_t at) const {
        const auto digitAt = [&](std::size_t index) {
            return index < word.size() &&
                   (std::isdigit(static_cast<unsigned char>(word[index])) != 0 ||
                    word[index] == '.');
        };
        if (at < word.size() && word[at] == '-') {
            return digitAt(at + 1);
        }
        return digitAt(at);
    }

    ThemeColor withLightness(const RgbColor& rgb, bool terminalPalette) const {
        ThemeColor color;
        color.rgb = rgb;
        color.lightness = m_oklab.rgbToOklab(rgb)[0];
        color.terminalPalette = terminalPalette;
        return color;
    }

    /** The xterm 256-color palette: 16 basic colors, a 6x6x6 cube and 24 grays. */
    RgbColor paletteColor(int index) const {
        if (index < 16) {
            const std::array<std::array<int, 3>, 16> basic{std::array<int, 3>{0, 0, 0},
                                                           {128, 0, 0},
                                                           {0, 128, 0},
                                                           {128, 128, 0},
                                                           {0, 0, 128},
                                                           {128, 0, 128},
                                                           {0, 128, 128},
                                                           {192, 192, 192},
                                                           {128, 128, 128},
                                                           {255, 0, 0},
                                                           {0, 255, 0},
                                                           {255, 255, 0},
                                                           {0, 0, 255},
                                                           {255, 0, 255},
                                                           {0, 255, 255},
                                                           {255, 255, 255}};
            return RgbColor{static_cast<double>(basic[index][0]),
                            static_cast<double>(basic[index][1]),
                            static_cast<double>(basic[index][2])};
        }
        if (index < 232) {
            const std::array<int, 6> cube{0, 95, 135, 175, 215, 255};
            const int offset = index - 16;
            return RgbColor{static_cast<double>(cube[offset / 36]),
                            static_cast<double>(cube[(offset % 36) / 6]),
                            static_cast<double>(cube[offset % 6])};
        }
        const double gray = 8 + (index - 232) * 10;
        return RgbColor{gray, gray, gray};
    }

    OklabConverter m_oklab;
};
