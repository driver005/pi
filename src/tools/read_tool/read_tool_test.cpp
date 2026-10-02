#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.base.base64_codec;
import pi.base.posix_file_system;
import pi.tools.read_tool;

class ReadToolTest : public testing::Test {
protected:
    std::string write(const std::string& name, const std::string& content) {
        EXPECT_TRUE(m_fs.writeFile(m_dir + "/" + name, content).has_value());
        return name;
    }

    Result<AgentToolResult> read(const Json& params) {
        return m_tool.execute("c1", params, nullptr, nullptr);
    }

    std::string textOf(const Result<AgentToolResult>& result, std::size_t index = 0) {
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
        return std::get<TextContent>(result->content[index]).text;
    }

    std::string numbered(int count) {
        std::string out;
        for (int i = 1; i <= count; ++i) {
            out += "line" + std::to_string(i) + (i < count ? "\n" : "");
        }
        return out;
    }

    PosixFileSystem m_fs;
    Base64Codec m_base64;
    std::string m_dir = [] {
        const std::string dir = std::string(std::getenv("TEST_TMPDIR")) + "/read_" +
                                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }();
    ReadTool m_tool{m_fs, m_base64, m_dir};
};

TEST_F(ReadToolTest, ReadsSmallFileWholly) {
    write("a.txt", "one\ntwo\nthree");
    EXPECT_EQ(textOf(read({{"path", "a.txt"}})), "one\ntwo\nthree");
}

TEST_F(ReadToolTest, OffsetAndLimitWithContinuationHint) {
    write("a.txt", numbered(10));
    const auto result = read({{"path", "a.txt"}, {"offset", 3}, {"limit", 2}});
    EXPECT_EQ(textOf(result), "line3\nline4\n\n[6 more lines in file. Use offset=5 to continue.]");
}

TEST_F(ReadToolTest, OffsetBeyondEndIsError) {
    write("a.txt", "x\ny");
    const auto result = read({{"path", "a.txt"}, {"offset", 9}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Offset 9 is beyond end of file (2 lines total)");
}

TEST_F(ReadToolTest, TruncatesByLinesWithNextOffset) {
    write("big.txt", numbered(2500));
    const auto result = read({{"path", "big.txt"}});
    const std::string text = textOf(result);
    EXPECT_NE(text.find("[Showing lines 1-2000 of 2500. Use offset=2001 to continue.]"), std::string::npos);
    EXPECT_TRUE(result->details["truncation"]["truncated"].get<bool>());
}

TEST_F(ReadToolTest, FirstLineTooLongPointsToBash) {
    write("wide.txt", std::string(60000, 'x') + "\nsecond");
    const std::string text = textOf(read({{"path", "wide.txt"}}));
    EXPECT_NE(text.find("[Line 1 is 58.6KB, exceeds 50.0KB limit. Use bash: sed -n '1p' wide.txt | head -c 51200]"),
              std::string::npos);
}

TEST_F(ReadToolTest, MissingAndUnreadableFilesGiveNodeStyleErrors) {
    const auto missing = read({{"path", "nope.txt"}});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "ENOENT: no such file or directory, access '" + m_dir + "/nope.txt'");
}

TEST_F(ReadToolTest, ImagesAreReturnedAsAttachments) {
    write("p.png", std::string("\x89PNG\r\n\x1a\n", 8) + "datadatadata");
    const auto result = read({{"path", "p.png"}});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->content.size(), 2U);
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "Read image file [image/png]");
    const auto& image = std::get<ImageContent>(result->content[1]);
    EXPECT_EQ(image.mimeType, "image/png");
    EXPECT_EQ(m_base64.decode(image.data).value().substr(1, 3), "PNG");
}

TEST_F(ReadToolTest, ResolvesTildeAtPrefixAndAbsolutePaths) {
    write("a.txt", "hi");
    EXPECT_EQ(textOf(read({{"path", "@a.txt"}})), "hi");
    EXPECT_EQ(textOf(read({{"path", m_dir + "/a.txt"}})), "hi");
}

TEST_F(ReadToolTest, AbortedSignalFailsImmediately) {
    write("a.txt", "hi");
    auto signal = std::make_shared<AbortSignal>();
    signal->abort();
    const auto result = m_tool.execute("c1", {{"path", "a.txt"}}, signal, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Operation aborted");
}
