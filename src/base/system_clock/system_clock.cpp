#include "src/base/system_clock/system_clock.h"

#include <chrono>

std::int64_t SystemClock::nowMs() const {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
}
