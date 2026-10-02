#include "src/base/stderr_logger/stderr_logger.h"

#include <gtest/gtest.h>

class FixedClock : public IClock {
public:
    std::int64_t nowMs() const override {
        return 1'700'000'000'123;
    }
};

TEST(StderrLoggerTest, WritesFormattedLineAtOrAboveMinLevel) {
    FixedClock clock;
    StderrLogger logger(clock, LogLevel::Info);
    testing::internal::CaptureStderr();
    logger.log(LogLevel::Debug, "hidden");
    logger.log(LogLevel::Warn, "shown");
    const std::string out = testing::internal::GetCapturedStderr();
    EXPECT_EQ(out, "2023-11-14T22:13:20.123Z [warn] shown\n");
}
