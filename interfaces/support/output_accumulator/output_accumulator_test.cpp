#include <gtest/gtest.h>

import std;
import pi.base.boring_crypto;
import pi.base.posix_file_system;
import pi.support.output_accumulator;

class OutputAccumulatorTest : public testing::Test {
protected:
    PosixFileSystem m_fs;
    BoringCrypto m_crypto;
};

TEST_F(OutputAccumulatorTest, SmallOutputStaysInMemory) {
    OutputAccumulator out(m_fs, m_crypto, "pi-test");
    out.append("hello\nwor");
    out.append("ld\n");
    out.finish();
    const OutputSnapshot snapshot = out.snapshot(true);
    EXPECT_EQ(snapshot.content, "hello\nworld\n");
    EXPECT_FALSE(snapshot.truncation.truncated);
    EXPECT_EQ(snapshot.truncation.totalLines, 2);
    EXPECT_FALSE(snapshot.fullOutputPath.has_value());
    EXPECT_EQ(out.readFullOutput(1000).content, "hello\nworld\n");
}

TEST_F(OutputAccumulatorTest, ManyLinesTruncateToTailAndSpillToFile) {
    OutputAccumulator out(m_fs, m_crypto, "pi-test", 10, 1000);
    std::string all;
    for (int i = 1; i <= 25; ++i) {
        const std::string line = "line" + std::to_string(i) + "\n";
        all += line;
        out.append(line);
    }
    out.finish();
    const OutputSnapshot snapshot = out.snapshot(true);
    EXPECT_TRUE(snapshot.truncation.truncated);
    EXPECT_EQ(snapshot.truncation.truncatedBy, "lines");
    EXPECT_EQ(snapshot.truncation.totalLines, 25);
    EXPECT_EQ(snapshot.truncation.outputLines, 10);
    EXPECT_EQ(snapshot.content.substr(0, 6), "line16");
    ASSERT_TRUE(snapshot.fullOutputPath.has_value());
    EXPECT_EQ(m_fs.readFile(*snapshot.fullOutputPath).value(), all);
}

TEST_F(OutputAccumulatorTest, ByteLimitAndPartialLastLine) {
    OutputAccumulator out(m_fs, m_crypto, "pi-test", 100, 20);
    out.append(std::string(100, 'x'));
    out.finish();
    const OutputSnapshot snapshot = out.snapshot(false);
    EXPECT_TRUE(snapshot.truncation.truncated);
    EXPECT_TRUE(snapshot.truncation.lastLinePartial);
    EXPECT_EQ(snapshot.content.size(), 20U);
    EXPECT_EQ(out.lastLineBytes(), 100);
    ASSERT_TRUE(snapshot.fullOutputPath.has_value() || out.tempFilePath().empty());
}

TEST_F(OutputAccumulatorTest, ReadFullOutputKeepsHeadAndTailAroundMarker) {
    OutputAccumulator out(m_fs, m_crypto, "pi-test", 10, 100);
    std::string data;
    for (int i = 0; i < 100; ++i) {
        data += "0123456789";
    }
    out.append(data);
    out.finish();
    const FullOutput full = out.readFullOutput(200);
    EXPECT_TRUE(full.truncated);
    EXPECT_NE(full.content.find("[... 800 bytes omitted ...]"), std::string::npos);
    EXPECT_EQ(full.content.substr(0, 10), "0123456789");
}
