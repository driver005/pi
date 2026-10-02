#include <gtest/gtest.h>

import std;
import pi.platform_services;

TEST(PlatformServicesTest, ProvidesWorkingServices) {
    PlatformServices services(2);
    EXPECT_GT(services.clock().nowMs(), 0);
    EXPECT_NE(services.ids().next(), services.ids().next());
    EXPECT_TRUE(services.files().exists("/"));
    std::promise<int> done;
    services.executor().submit([&done] { done.set_value(7); });
    EXPECT_EQ(done.get_future().get(), 7);
    EXPECT_EQ(services.base64().encode("hi"), "aGk=");
}
