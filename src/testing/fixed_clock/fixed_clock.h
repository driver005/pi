#pragma once

#include <cstdint>

#include "interfaces/platform/i_clock/i_clock.h"

/** IClock whose time only moves when a test advances it. */
class FixedClock : public IClock {
public:
    explicit FixedClock(std::int64_t nowMs = 1'700'000'000'000) : m_now(nowMs) {}

    std::int64_t nowMs() const override {
        return m_now;
    }

    void advance(std::int64_t ms) {
        m_now += ms;
    }

private:
    std::int64_t m_now;
};
