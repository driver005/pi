export module pi.types.summary_result;

import std;
export import pi.types.usage;

/** Summary text and the provider usage spent producing it. */
export struct SummaryResult {
    std::string text;
    Usage usage;
};
