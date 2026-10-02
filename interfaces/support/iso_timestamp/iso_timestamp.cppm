module;

#include <cstdint>

export module pi.support.iso_timestamp;

import std;

/** JavaScript-style ISO-8601 timestamps ("2025-01-31T12:34:56.789Z", always UTC, milliseconds). */
export class IsoTimestamp {
public:
    std::string format(std::int64_t epochMs) const;

    /** Accepts "YYYY-MM-DDTHH:MM:SS[.fff]Z" and the same with a numeric offset; nullopt otherwise. */
    std::optional<std::int64_t> parse(const std::string& text) const;

private:
    std::optional<int> digits(const std::string& text, std::size_t start, std::size_t count) const;
};

std::string IsoTimestamp::format(std::int64_t epochMs) const {
    const std::chrono::sys_time<std::chrono::milliseconds> time{std::chrono::milliseconds{epochMs}};
    const auto days = std::chrono::floor<std::chrono::days>(time);
    const std::chrono::year_month_day date{days};
    const auto remainder = std::chrono::duration_cast<std::chrono::milliseconds>(time - days);
    const std::chrono::hh_mm_ss<std::chrono::milliseconds> clock{remainder};
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                  static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()),
                  static_cast<int>(clock.seconds().count()), static_cast<int>(clock.subseconds().count()));
    return buffer;
}

std::optional<int> IsoTimestamp::digits(const std::string& text, std::size_t start,
                                        std::size_t count) const {
    if (start + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = start; i < start + count; ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
            return std::nullopt;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

std::optional<std::int64_t> IsoTimestamp::parse(const std::string& text) const {
    const auto year = digits(text, 0, 4);
    const auto month = digits(text, 5, 2);
    const auto day = digits(text, 8, 2);
    const auto hour = digits(text, 11, 2);
    const auto minute = digits(text, 14, 2);
    const auto second = digits(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second || text.size() < 20 || text[4] != '-' ||
        text[7] != '-' || (text[10] != 'T' && text[10] != ' ') || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    std::size_t index = 19;
    int millis = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        int scale = 100;
        while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
            millis += (text[index] - '0') * scale;
            scale /= 10;
            ++index;
        }
    }
    std::int64_t offsetMinutes = 0;
    if (index < text.size() && text[index] == 'Z') {
        ++index;
    } else if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        const int sign = text[index] == '+' ? 1 : -1;
        const auto offsetHour = digits(text, index + 1, 2);
        const auto offsetMinute = digits(text, index + 1 + (text.size() > index + 3 && text[index + 3] == ':' ? 3 : 2), 2);
        if (!offsetHour || !offsetMinute) {
            return std::nullopt;
        }
        offsetMinutes = sign * (*offsetHour * 60 + *offsetMinute);
        index = text.size();
    }
    if (index != text.size()) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }
    const auto seconds = std::chrono::sys_days{date}.time_since_epoch() + std::chrono::hours{*hour} +
                         std::chrono::minutes{*minute} + std::chrono::seconds{*second};
    return std::chrono::duration_cast<std::chrono::milliseconds>(seconds).count() + millis -
           offsetMinutes * 60000;
}
