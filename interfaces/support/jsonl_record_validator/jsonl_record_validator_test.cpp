#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.jsonl_record_validator;

class JsonlRecordValidatorTest : public ::testing::Test {
protected:
    void expectCorrupt(const Result<Json>& result, const std::string& text) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, "jsonl_corruption");
        EXPECT_NE(result.error().message.find(text), std::string::npos) << result.error().message;
    }

    JsonlRecordValidator m_validator;
};

TEST_F(JsonlRecordValidatorTest, AcceptsAWellFormedCommitMarker) {
    const auto marker = m_validator.parseMainMarker(
        R"({"format":1,"type":"commit","seq":3,"writes":[{"type":"entry","value":{"id":4}},{"type":"task.sidecar","id":5,"ordinal":0}]})", 7);
    ASSERT_TRUE(marker);
    EXPECT_EQ((*marker)["seq"], 3);
}

TEST_F(JsonlRecordValidatorTest, RejectsMalformedAndMisshapenMarkers) {
    expectCorrupt(m_validator.parseMainMarker("{not json", 2), "Malformed complete main.jsonl line 2");
    expectCorrupt(m_validator.parseMainMarker(R"({"format":2,"type":"commit","seq":1,"writes":[]})", 1), "Invalid commit marker");
    expectCorrupt(m_validator.parseMainMarker(R"({"format":1,"type":"commit","seq":0,"writes":[]})", 1), "Invalid commit marker");
    expectCorrupt(m_validator.parseMainMarker(R"({"format":1,"type":"commit","seq":1,"writes":{}})", 1), "Invalid commit marker");
    expectCorrupt(m_validator.parseMainMarker(R"({"format":1,"type":"commit","seq":1.5,"writes":[]})", 1), "Invalid commit marker");
}

TEST_F(JsonlRecordValidatorTest, ChecksEveryKindOfMainWrite) {
    auto marker = [&](const std::string& write) {
        return m_validator.parseMainMarker(R"({"format":1,"type":"commit","seq":1,"writes":[)" + write + "]}", 1);
    };
    EXPECT_TRUE(marker(R"({"type":"conversation","value":{"id":1}})"));
    EXPECT_TRUE(marker(R"({"type":"task","value":{"id":2,"state":{"status":"terminal"}}})"));
    EXPECT_TRUE(marker(R"({"type":"document.retire","id":3})"));
    EXPECT_TRUE(marker(R"({"type":"document.create","record":{"id":4},"ordinal":0})"));
    EXPECT_TRUE(marker(R"({"type":"document.change","id":4,"ordinal":1})"));
    expectCorrupt(marker(R"({"type":"entry","value":{}})"), "Invalid entry write");
    expectCorrupt(marker(R"({"type":"task","value":{"id":2,"state":{"status":"pending"}}})"), "Invalid terminal task write");
    expectCorrupt(marker(R"({"type":"document.retire"})"), "Invalid document retirement");
    expectCorrupt(marker(R"({"type":"task.sidecar","id":2,"ordinal":-1})"), "Invalid task sidecar write");
    expectCorrupt(marker(R"({"type":"document.create","record":{"id":4}})"), "Invalid document creation");
    expectCorrupt(marker(R"({"type":"document.change","id":4})"), "Invalid document change");
    expectCorrupt(marker(R"({"type":"bogus"})"), "Unknown write type");
    expectCorrupt(marker("3"), "Invalid write");
}

TEST_F(JsonlRecordValidatorTest, ChecksSidecarRecords) {
    auto record = [&](const std::string& payload) {
        return m_validator.parseSidecarRecord(R"({"format":1,"type":"record","seq":2,"ordinal":0,"payload":)" + payload + "}", "doc-4.jsonl", 3);
    };
    EXPECT_TRUE(record(R"({"type":"task","value":{"id":5,"state":{"status":"pending"}}})"));
    EXPECT_TRUE(record(R"({"type":"document","id":4,"content":{"kind":"base","version":1,"value":{}}})"));
    EXPECT_TRUE(record(R"({"type":"document","id":4,"content":{"kind":"delta","version":2,"ops":[]}})"));
    expectCorrupt(record(R"({"type":"task","value":{"id":5,"state":{"status":"terminal"}}})"), "Invalid live task record");
    expectCorrupt(record(R"({"type":"document","id":4,"content":{"kind":"base","version":0,"value":{}}})"), "Invalid document content");
    expectCorrupt(record(R"({"type":"document","id":4,"content":{"kind":"delta","version":1}})"), "Invalid document content");
    expectCorrupt(record(R"({"type":"document"})"), "Invalid document record");
    expectCorrupt(record(R"({"type":"other"})"), "Unknown sidecar record type");
    expectCorrupt(m_validator.parseSidecarRecord("[]", "task-5.jsonl", 1), "Invalid sidecar record in task-5.jsonl line 1");
}
