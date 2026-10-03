module;

#include <cstdint>

export module pi.support.http_date_parser;

import std;

/** Parses the IMF-fixdate form of HTTP dates ("Sun, 06 Nov 1994 08:49:37 GMT"). */
export class HttpDateParser {
public:
    /** Epoch milliseconds, or nullopt when the text is not an HTTP date. */
    std::optional<std::int64_t> parseMs(const std::string& text) const {
        const std::string months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const auto comma = text.find(',');
        if (comma == std::string::npos || text.size() < comma + 21) {
            return std::nullopt;
        }
        std::istringstream stream(text.substr(comma + 1));
        int day = 0;
        int year = 0;
        int hour = 0;
        int minute = 0;
        int second = 0;
        char colon = 0;
        std::string monthName;
        stream >> day >> monthName >> year >> hour >> colon >> minute >> colon >> second;
        const auto month = months.find(monthName);
        if (stream.fail() || monthName.size() != 3 || month == std::string::npos || month % 3 != 0) {
            return std::nullopt;
        }
        const std::chrono::year_month_day date{std::chrono::year{year},
                                               std::chrono::month{static_cast<unsigned>(month / 3 + 1)},
                                               std::chrono::day{static_cast<unsigned>(day)}};
        if (!date.ok()) {
            return std::nullopt;
        }
        const auto seconds = std::chrono::sys_days{date}.time_since_epoch() + std::chrono::hours{hour} +
                             std::chrono::minutes{minute} + std::chrono::seconds{second};
        return std::chrono::duration_cast<std::chrono::milliseconds>(seconds).count();
    }
};
