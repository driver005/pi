export module pi.support.js_number_format;

import std;

/** Number formatting the way JavaScript does it, so reports read the same as those of the TypeScript runner. */
export class JsNumberFormat {
public:
    /** `Number.prototype.toFixed`: a tie rounds away from zero (printf rounds it to even), and a negative number keeps its sign even when it rounds to zero. */
    std::string toFixed(double value, int digits) const {
        const bool negative = value < 0;
        const double magnitude = std::abs(value);
        // An exact tie (the double is a multiple of 2^-(digits+1) whose last digit is 5) rounds up here, to even in printf.
        const double scaled = std::ldexp(magnitude, digits + 1);
        const bool tie = scaled < 1e15 && scaled == std::floor(scaled) && std::format("{:.{}f}", magnitude, digits + 1).back() == '5';
        const std::string text = std::format("{:.{}f}", tie ? std::nextafter(magnitude, std::numeric_limits<double>::infinity()) : magnitude, digits);
        return negative ? "-" + text : text;
    }

    /** `Number((value).toPrecision(15))`: the value rounded to 15 significant digits, which hides binary noise like 0.30000000000000004. */
    double roundTo15(double value) const {
        const std::string text = std::format("{:.14e}", value);
        double out = value;
        std::from_chars(text.data(), text.data() + text.size(), out);
        return out;
    }
};
