#include <gtest/gtest.h>

#include <unistd.h>

import std;
import pi.base.stdio_byte_input;

TEST(StdioByteInputTest, ReadsChunksUntilTheWriterCloses) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    StdioByteInput input(fds[0]);
    ASSERT_EQ(::write(fds[1], "hello\n", 6), 6);
    EXPECT_EQ(input.read(), "hello\n");
    ::close(fds[1]);
    EXPECT_FALSE(input.read().has_value());
    ::close(fds[0]);
}
