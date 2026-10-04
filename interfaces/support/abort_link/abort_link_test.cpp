#include <gtest/gtest.h>

import std;
import pi.support.abort_link;

TEST(AbortLinkTest, AbortsWhenAnySourceAborts) {
    AbortSignal first;
    AbortSignal second;
    AbortLink link({&first, &second, nullptr});
    EXPECT_FALSE(link.signal()->aborted());
    second.abort();
    EXPECT_TRUE(link.signal()->aborted());
    EXPECT_FALSE(first.aborted());
}

TEST(AbortLinkTest, AlreadyAbortedSourceAbortsImmediately) {
    AbortSignal source;
    source.abort();
    AbortLink link({&source});
    EXPECT_TRUE(link.signal()->aborted());
}

TEST(AbortLinkTest, DestroyedLinkNoLongerListens) {
    AbortSignal source;
    {
        AbortLink link({&source});
    }
    source.abort();
    EXPECT_TRUE(source.aborted());
}
