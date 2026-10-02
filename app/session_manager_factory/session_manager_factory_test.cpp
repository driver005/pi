#include <gtest/gtest.h>

import std;
import pi.session_manager_factory;
import pi.testing.fake_file_system;
import pi.testing.fixed_clock;
import pi.testing.sequential_id_generator;

TEST(SessionManagerFactoryTest, CreatesOpenedManagers) {
    FakeFileSystem files;
    FixedClock clock;
    SequentialIdGenerator ids{"s"};
    SessionManagerFactory factory(files, clock, ids);
    SessionManagerOptions options;
    options.cwd = "/work";
    options.persist = false;
    auto manager = factory.create(options);
    ASSERT_TRUE(manager.has_value());
    EXPECT_EQ((*manager)->cwd(), "/work");
    EXPECT_FALSE((*manager)->sessionId().empty());
    EXPECT_FALSE((*manager)->isPersisted());
}
