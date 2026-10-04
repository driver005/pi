#include <gtest/gtest.h>

#include <unistd.h>

import std;
import pi.base.stdio_byte_output;

TEST(StdioByteOutputTest, WritesEverythingToTheDescriptor) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    StdioByteOutput output(fds[1]);
    output.write("one ");
    output.write("two\n");
    output.flush();
    ::close(fds[1]);
    char buffer[64];
    const ssize_t count = ::read(fds[0], buffer, sizeof(buffer));
    ASSERT_GT(count, 0);
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(count)), "one two\n");
    ::close(fds[0]);
}
