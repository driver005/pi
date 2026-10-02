#pragma once

/** Dollar cost of one response, split like Usage. */
struct UsageCost {
    double input = 0;
    double output = 0;
    double cacheRead = 0;
    double cacheWrite = 0;
    double total = 0;
};
