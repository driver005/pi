#pragma once

#include <cstdint>

/** Source of wall-clock time; injected so time-dependent modules stay testable. */
class IClock {
public:
    virtual ~IClock() = default;

    /** Milliseconds since the Unix epoch. */
    virtual std::int64_t nowMs() const = 0;
};
