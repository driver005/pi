export module pi.support.usage_combiner;

import std;
export import pi.types.usage;

/** Adds usages from several model calls into one. */
export class UsageCombiner {
public:
    /** Sum of both; the optional token splits are kept when either side reports them. */
    Usage combine(const Usage& first, const Usage& second) const;
};

Usage UsageCombiner::combine(const Usage& first, const Usage& second) const {
    Usage out;
    out.input = first.input + second.input;
    out.output = first.output + second.output;
    out.cacheRead = first.cacheRead + second.cacheRead;
    out.cacheWrite = first.cacheWrite + second.cacheWrite;
    if (first.cacheWrite1h || second.cacheWrite1h) {
        out.cacheWrite1h = first.cacheWrite1h.value_or(0) + second.cacheWrite1h.value_or(0);
    }
    if (first.reasoning || second.reasoning) {
        out.reasoning = first.reasoning.value_or(0) + second.reasoning.value_or(0);
    }
    out.totalTokens = first.totalTokens + second.totalTokens;
    out.cost.input = first.cost.input + second.cost.input;
    out.cost.output = first.cost.output + second.cost.output;
    out.cost.cacheRead = first.cost.cacheRead + second.cost.cacheRead;
    out.cost.cacheWrite = first.cost.cacheWrite + second.cost.cacheWrite;
    out.cost.total = first.cost.total + second.cost.total;
    return out;
}
