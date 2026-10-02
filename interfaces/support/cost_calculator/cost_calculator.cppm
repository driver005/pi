export module pi.support.cost_calculator;

import std;
export import pi.types.model;
export import pi.types.usage;

/** Dollar cost of a response from the model's price list. Port of calculateCost in models.ts. */
export class CostCalculator {
public:
    /** Fills usage.cost in place and returns it. Tiers apply by total input tokens. */
    UsageCost calculate(const Model& model, Usage& usage) const;
};

UsageCost CostCalculator::calculate(const Model& model, Usage& usage) const {
    const std::int64_t inputTokens = usage.input + usage.cacheRead + usage.cacheWrite;
    double input = model.cost.input;
    double output = model.cost.output;
    double cacheRead = model.cost.cacheRead;
    double cacheWrite = model.cost.cacheWrite;
    std::int64_t matched = -1;
    for (const auto& tier : model.cost.tiers) {
        if (inputTokens > tier.inputTokensAbove && tier.inputTokensAbove > matched) {
            input = tier.input;
            output = tier.output;
            cacheRead = tier.cacheRead;
            cacheWrite = tier.cacheWrite;
            matched = tier.inputTokensAbove;
        }
    }
    // 1h cache writes cost twice the base input rate.
    const std::int64_t longWrite = usage.cacheWrite1h.value_or(0);
    const std::int64_t shortWrite = usage.cacheWrite - longWrite;
    usage.cost.input = input / 1e6 * static_cast<double>(usage.input);
    usage.cost.output = output / 1e6 * static_cast<double>(usage.output);
    usage.cost.cacheRead = cacheRead / 1e6 * static_cast<double>(usage.cacheRead);
    usage.cost.cacheWrite = (cacheWrite * static_cast<double>(shortWrite) +
                             input * 2 * static_cast<double>(longWrite)) /
                            1e6;
    usage.cost.total =
        usage.cost.input + usage.cost.output + usage.cost.cacheRead + usage.cost.cacheWrite;
    return usage.cost;
}
