#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

import std;
import pi.export_command;
import pi.types.json;

class ExportCommandTest : public testing::Test {
protected:
    ExportCommandTest() {
        m_dir = std::string(std::getenv("TEST_TMPDIR")) + "/export_command_" + testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_dir);
        std::filesystem::create_directories(m_dir + "/agent");
        std::filesystem::create_directories(m_dir + "/work");
        m_services = std::make_unique<CodingServices>(m_dir + "/agent", m_dir + "/agent/catalog", true);
        std::ofstream(m_dir + "/session.jsonl")
            << R"({"type":"session","version":3,"id":"sess-1","timestamp":"2026-01-01T00:00:00.000Z","cwd":")" << m_dir << R"(/work"})" << "\n"
            << R"({"type":"message","id":"e1","parentId":null,"timestamp":"2026-01-01T00:00:01.000Z","message":{"role":"user","content":"export me","timestamp":1}})" << "\n";
    }

    int run(const std::vector<std::string>& arguments, const std::string& theme = "") {
        CommandLine line;
        line.command = "export";
        line.arguments = arguments;
        line.exportTheme = theme;
        line.options.cwd = m_dir + "/work";
        line.options.agentDir = m_dir + "/agent";
        m_out.str("");
        m_err.str("");
        ExportCommand command(*m_services, m_out, m_err);
        return command.run(line);
    }

    std::string read(const std::string& path) {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::string m_dir;
    std::unique_ptr<CodingServices> m_services;
    std::ostringstream m_out;
    std::ostringstream m_err;
};

TEST_F(ExportCommandTest, WritesThePageNextToTheWorkingDirectoryByDefault) {
    ASSERT_EQ(run({m_dir + "/session.jsonl"}), 0) << m_err.str();
    const std::string target = m_dir + "/work/pi-session-session.html";
    EXPECT_EQ(m_out.str(), "Exported to: " + target + "\n");
    const std::string html = read(target);
    EXPECT_NE(html.find("<title>Session Export</title>"), std::string::npos);
    EXPECT_EQ(html.find("{{"), std::string::npos);
    const std::size_t start = html.find("<script id=\"session-data\" type=\"application/json\">") + 50;
    const auto decoded = m_services->platform().base64().decode(html.substr(start, html.find("</script>", start) - start));
    ASSERT_TRUE(decoded.has_value());
    const Json data = Json::parse(*decoded);
    EXPECT_EQ(data["header"]["id"], "sess-1");
    EXPECT_EQ(data["entries"][0]["id"], "e1");
    EXPECT_EQ(data["leafId"], "e1");
    EXPECT_FALSE(data.contains("systemPrompt"));
}

TEST_F(ExportCommandTest, TakesAnOutputPathAndATheme) {
    ASSERT_EQ(run({m_dir + "/session.jsonl", "out.html"}, "light"), 0) << m_err.str();
    EXPECT_NE(read(m_dir + "/work/out.html").find("--exportPageBg: #efeeee;"), std::string::npos);
}

TEST_F(ExportCommandTest, FailuresAreReported) {
    EXPECT_EQ(run({m_dir + "/missing.jsonl"}), 1);
    EXPECT_NE(m_err.str().find("File not found"), std::string::npos);
    EXPECT_EQ(run({m_dir + "/session.jsonl"}, "mauve"), 1);
    EXPECT_NE(m_err.str().find("Unknown theme \"mauve\""), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(m_dir + "/work/pi-session-session.html"));
}
