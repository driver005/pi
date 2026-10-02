#include <gtest/gtest.h>

import std;
import pi.support.plugin_discovery;
import pi.testing.fake_file_system;

TEST(PluginDiscoveryTest, FindsLibrariesSortedAndIgnoresOtherFiles) {
    FakeFileSystem files;
    files.createDirectories("/plugins/nested.so");
    files.writeFile("/plugins/b.so", "");
    files.writeFile("/plugins/a.dylib", "");
    files.writeFile("/plugins/readme.md", "");
    PluginDiscovery discovery(files);
    EXPECT_EQ(discovery.discover("/plugins"), (std::vector<std::string>{"/plugins/a.dylib", "/plugins/b.so"}));
    EXPECT_EQ(discovery.discover("/plugins/"), (std::vector<std::string>{"/plugins/a.dylib", "/plugins/b.so"}));
}

TEST(PluginDiscoveryTest, MissingDirectoriesHaveNoPlugins) {
    FakeFileSystem files;
    EXPECT_TRUE(PluginDiscovery(files).discover("/nowhere").empty());
}
