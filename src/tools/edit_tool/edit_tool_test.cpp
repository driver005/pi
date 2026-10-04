#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.base.posix_file_system;
import pi.tools.edit_tool;
import pi.tools.file_mutation_queue;

class EditToolTest : public testing::Test {
protected:
    Result<AgentToolResult> edit(const Json& args) {
        return m_tool.execute("c1", m_tool.prepareArguments(args), nullptr, nullptr);
    }

    std::string file(const std::string& name) {
        return m_fs.readFile(m_dir + "/" + name).value();
    }

    PosixFileSystem m_fs;
    FileMutationQueue m_queue{m_fs};
    std::string m_dir = [] {
        const std::string dir = std::string(std::getenv("TEST_TMPDIR")) + "/edit_" +
                                testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }();
    EditTool m_tool{m_fs, m_queue, m_dir};
};

TEST_F(EditToolTest, ReplacesTextAndReportsDiff) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/a.txt", "one\ntwo\nthree\n").has_value());
    const auto result = edit({{"path", "a.txt"}, {"edits", Json::array({{{"oldText", "two"}, {"newText", "2"}}})}});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "Successfully replaced 1 block(s) in a.txt.");
    EXPECT_EQ(file("a.txt"), "one\n2\nthree\n");
    EXPECT_EQ(result->details["firstChangedLine"], 2);
    EXPECT_NE(result->details["diff"].get<std::string>().find("-2 two"), std::string::npos);
    EXPECT_NE(result->details["patch"].get<std::string>().find("--- a.txt"), std::string::npos);
}

TEST_F(EditToolTest, PreservesBomAndCrlf) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/w.txt", "\xEF\xBB\xBF" "a\r\nb\r\nc\r\n").has_value());
    ASSERT_TRUE(edit({{"path", "w.txt"}, {"edits", Json::array({{{"oldText", "b"}, {"newText", "B"}}})}}).has_value());
    EXPECT_EQ(file("w.txt"), "\xEF\xBB\xBF" "a\r\nB\r\nc\r\n");
}

TEST_F(EditToolTest, MultipleEditsAreMatchedAgainstOriginal) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/m.txt", "a b c").has_value());
    const auto result = edit({{"path", "m.txt"},
                              {"edits", Json::array({{{"oldText", "a"}, {"newText", "b"}}, {{"oldText", "c"}, {"newText", "a"}}})}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(file("m.txt"), "b b a");
}

TEST_F(EditToolTest, NotFoundAndMissingFileErrors) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/a.txt", "abc").has_value());
    const auto notFound = edit({{"path", "a.txt"}, {"edits", Json::array({{{"oldText", "zzz"}, {"newText", "x"}}})}});
    ASSERT_FALSE(notFound.has_value());
    EXPECT_NE(notFound.error().message.find("Could not find the exact text in a.txt"), std::string::npos);
    EXPECT_EQ(file("a.txt"), "abc");
    const auto missing = edit({{"path", "nope.txt"}, {"edits", Json::array({{{"oldText", "a"}, {"newText", "b"}}})}});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "Could not edit file: nope.txt. Error code: ENOENT.");
}

TEST_F(EditToolTest, EmptyEditsIsInvalidInput) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/a.txt", "abc").has_value());
    const auto result = edit({{"path", "a.txt"}, {"edits", Json::array()}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Edit tool input is invalid. edits must contain at least one replacement.");
}

TEST_F(EditToolTest, PrepareArgumentsAcceptsLegacyAndStringForms) {
    const Json legacy = m_tool.prepareArguments({{"path", "a"}, {"oldText", "x"}, {"newText", "y"}});
    EXPECT_EQ(legacy["edits"], Json::parse(R"([{"oldText":"x","newText":"y"}])"));
    EXPECT_FALSE(legacy.contains("oldText"));
    const Json stringForm = m_tool.prepareArguments({{"path", "a"}, {"edits", R"([{"oldText":"x","newText":"y"}])"}});
    EXPECT_EQ(stringForm["edits"], Json::parse(R"([{"oldText":"x","newText":"y"}])"));
    const Json single = m_tool.prepareArguments({{"path", "a"}, {"edits", {{"oldText", "x"}, {"newText", "y"}}}});
    EXPECT_TRUE(single["edits"].is_array());
    EXPECT_EQ(single["edits"].size(), 1U);
}

TEST_F(EditToolTest, FuzzyMatchEditsSmartQuotes) {
    ASSERT_TRUE(m_fs.writeFile(m_dir + "/q.txt", "say \xE2\x80\x9Chi\xE2\x80\x9D\nkeep\n").has_value());
    ASSERT_TRUE(edit({{"path", "q.txt"}, {"edits", Json::array({{{"oldText", "say \"hi\""}, {"newText", "say \"yo\""}}})}}).has_value());
    EXPECT_EQ(file("q.txt"), "say \"yo\"\nkeep\n");
}
