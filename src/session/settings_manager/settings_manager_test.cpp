#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstdlib>

import std;
import pi.base.posix_file_lock;
import pi.base.posix_file_system;
import pi.session.settings_manager;
import pi.testing.sequential_id_generator;

class SettingsManagerTest : public testing::Test {
protected:
    SettingsManagerTest() {
        m_root = std::string(std::getenv("TEST_TMPDIR")) + "/settings_" +
                 testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root + "/agent");
        std::filesystem::create_directories(m_root + "/project/.pi");
    }

    std::unique_ptr<SettingsManager> manager(bool trusted = true) {
        return std::make_unique<SettingsManager>(m_root + "/agent/settings.json",
                                                 m_root + "/project/.pi/settings.json", trusted, m_files,
                                                 m_lock, m_ids);
    }

    void write(const std::string& relative, const std::string& text) {
        ASSERT_TRUE(m_files.writeFile(m_root + "/" + relative, text).has_value());
    }

    Json read(const std::string& relative) { return Json::parse(m_files.readFile(m_root + "/" + relative).value()); }

    std::string m_root;
    PosixFileSystem m_files;
    PosixFileLock m_lock;
    SequentialIdGenerator m_ids{"device-"};
};

TEST_F(SettingsManagerTest, EmptyWhenNoFiles) {
    auto settings = manager();
    EXPECT_TRUE(settings->settings().empty());
    EXPECT_TRUE(settings->drainErrors().empty());
    EXPECT_EQ(settings->view().steeringMode(), "one-at-a-time");
}

TEST_F(SettingsManagerTest, ProjectOverridesGlobalAndNestedObjectsMerge) {
    write("agent/settings.json", R"({"defaultModel":"a","retry":{"enabled":true,"maxRetries":5}})");
    write("project/.pi/settings.json", R"({"defaultModel":"b","retry":{"maxRetries":1}})");
    auto settings = manager();
    EXPECT_EQ(settings->view().defaultModel(), "b");
    EXPECT_TRUE(settings->view().retryEnabled());
    EXPECT_EQ(settings->view().retryMaxRetries(), 1);
    EXPECT_EQ(settings->globalSettings()["defaultModel"], "a");
}

TEST_F(SettingsManagerTest, UntrustedProjectContributesNothingAndCannotBeWritten) {
    write("project/.pi/settings.json", R"({"defaultModel":"project"})");
    auto settings = manager(false);
    EXPECT_FALSE(settings->view().defaultModel().has_value());
    const auto denied = settings->setProject("defaultModel", Json("x"));
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().message, "Project is not trusted; refusing to write project settings");
    ASSERT_TRUE(settings->setProjectTrusted(true).has_value());
    EXPECT_EQ(settings->view().defaultModel(), "project");
}

TEST_F(SettingsManagerTest, SetGlobalCreatesFileAndKeepsOtherFieldsFromTheFile) {
    auto settings = manager();
    ASSERT_TRUE(settings->setGlobal("defaultProvider", Json("anthropic")).has_value());
    EXPECT_EQ(read("agent/settings.json")["defaultProvider"], "anthropic");
    // Another process edits a different field while we are running.
    write("agent/settings.json", R"({"defaultProvider":"anthropic","theme":"dark"})");
    ASSERT_TRUE(settings->setGlobal("defaultModel", Json("claude")).has_value());
    const Json file = read("agent/settings.json");
    EXPECT_EQ(file["theme"], "dark");
    EXPECT_EQ(file["defaultModel"], "claude");
    EXPECT_EQ(file["defaultProvider"], "anthropic");
}

TEST_F(SettingsManagerTest, NestedWritesKeepSiblingKeysInTheFile) {
    write("agent/settings.json", R"({"modelThinkingLevels":{"a/b":"low"}})");
    auto settings = manager();
    write("agent/settings.json", R"({"modelThinkingLevels":{"a/b":"low","c/d":"high"}})");
    ASSERT_TRUE(settings->setGlobalNested("modelThinkingLevels", "x/y", Json("medium")).has_value());
    const Json levels = read("agent/settings.json")["modelThinkingLevels"];
    EXPECT_EQ(levels["a/b"], "low");
    EXPECT_EQ(levels["c/d"], "high");
    EXPECT_EQ(levels["x/y"], "medium");
}

TEST_F(SettingsManagerTest, RemoveDeletesFieldFromFile) {
    write("agent/settings.json", R"({"a":1,"b":2})");
    auto settings = manager();
    ASSERT_TRUE(settings->removeGlobal("a").has_value());
    const Json file = read("agent/settings.json");
    EXPECT_FALSE(file.contains("a"));
    EXPECT_EQ(file["b"], 2);
}

TEST_F(SettingsManagerTest, ProjectWritesCreateDirectoryAndFile) {
    std::filesystem::remove_all(m_root + "/project/.pi");
    auto settings = manager();
    ASSERT_TRUE(settings->setProject("enabledModels", Json::parse(R"(["a"])")).has_value());
    EXPECT_EQ(read("project/.pi/settings.json")["enabledModels"][0], "a");
    EXPECT_EQ(settings->view().enabledModels()->size(), 1U);
}

TEST_F(SettingsManagerTest, CorruptFileIsReportedAndNeverOverwritten) {
    write("agent/settings.json", "{not json");
    auto settings = manager();
    const auto errors = settings->drainErrors();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].scope, "global");
    EXPECT_EQ(errors[0].path, m_root + "/agent/settings.json");
    EXPECT_TRUE(settings->drainErrors().empty());
    ASSERT_TRUE(settings->setGlobal("defaultModel", Json("x")).has_value());
    EXPECT_EQ(settings->view().defaultModel(), "x");
    EXPECT_EQ(m_files.readFile(m_root + "/agent/settings.json").value(), "{not json");
}

TEST_F(SettingsManagerTest, ReloadPicksUpExternalChanges) {
    write("agent/settings.json", R"({"defaultModel":"a"})");
    auto settings = manager();
    write("agent/settings.json", R"({"defaultModel":"b"})");
    EXPECT_EQ(settings->view().defaultModel(), "a");
    settings->reload();
    EXPECT_EQ(settings->view().defaultModel(), "b");
}

TEST_F(SettingsManagerTest, OverridesLayerOnTopWithoutPersisting) {
    write("agent/settings.json", R"({"transport":"sse"})");
    auto settings = manager();
    settings->applyOverrides(Json::parse(R"({"transport":"websocket","compaction":{"enabled":false}})"));
    EXPECT_EQ(settings->view().transport(), "websocket");
    EXPECT_FALSE(settings->view().compactionEnabled());
    settings->setGlobal("defaultModel", Json("m"));
    EXPECT_EQ(read("agent/settings.json")["transport"], "sse");
}

TEST_F(SettingsManagerTest, LegacyFormatsAreMigratedOnLoad) {
    write("agent/settings.json", R"({"queueMode":"all"})");
    auto settings = manager();
    EXPECT_EQ(settings->view().steeringMode(), "all");
}

TEST_F(SettingsManagerTest, DeviceIdCreatedOnceAndPersisted) {
    auto settings = manager();
    const auto first = settings->getOrCreateDeviceId();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, "device-1");
    EXPECT_EQ(*settings->getOrCreateDeviceId(), "device-1");
    EXPECT_EQ(read("agent/settings.json")["deviceId"], "device-1");
    auto again = manager();
    EXPECT_EQ(*again->getOrCreateDeviceId(), "device-1");
}
