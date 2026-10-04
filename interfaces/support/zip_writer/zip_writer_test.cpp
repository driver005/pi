#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

import std;
import pi.support.zip_writer;

TEST(ZipWriterTest, ProducesAnArchiveUnzipReads) {
    const std::string dir = std::string(std::getenv("TEST_TMPDIR")) + "/zip_writer";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ZipWriter writer;
    const std::string archive = writer.create({ZipEntry{"report.json", "{\"a\":1}\n"}, ZipEntry{"summary.md", std::string(5000, 'x')}, ZipEntry{"empty.txt", ""}}, 1'759'000'000'000);
    std::ofstream(dir + "/a.zip", std::ios::binary) << archive;
    ASSERT_EQ(archive.substr(0, 4), std::string("PK\x03\x04", 4));
    EXPECT_EQ(archive.substr(archive.size() - 22, 4), std::string("PK\x05\x06", 4));
    const std::string command = "cd " + dir + " && python3 -c \"import zipfile; z = zipfile.ZipFile('a.zip'); assert z.testzip() is None; assert z.read('report.json') == b'{\\\"a\\\":1}\\n'; assert len(z.read('summary.md')) == 5000; assert z.namelist() == ['report.json', 'summary.md', 'empty.txt']\"";
    EXPECT_EQ(std::system(command.c_str()), 0);
}
