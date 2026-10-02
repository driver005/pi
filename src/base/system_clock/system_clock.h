#pragma once

#include <cstdint>

#include "interfaces/platform/i_clock/i_clock.h"

/** IClock backed by std::chrono::system_clock. */
class SystemClock : public IClock {
public:
    std::int64_t nowMs() const override;
};
