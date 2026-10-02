export module pi.base.system_clock;

import std;
import pi.platform.i_clock;

/** IClock backed by std::chrono::system_clock. */
export class SystemClock : public IClock {
public:
    std::int64_t nowMs() const override {
        const auto since = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
    }
};
