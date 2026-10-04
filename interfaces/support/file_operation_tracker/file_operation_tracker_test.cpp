#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import pi.support.file_operation_tracker;

class FileOperationTrackerTest : public testing::Test {
protected:
    AgentMessage assistantCalling(std::vector<std::pair<std::string, Json>> calls) {
        AssistantMessage message;
        for (auto& [name, args] : calls) {
            ToolCall call;
            call.name = name;
            call.arguments = args;
            message.content.push_back(call);
        }
        return message;
    }

    FileOperationTracker m_tracker;
};

TEST_F(FileOperationTrackerTest, ExtractsReadWriteEditPaths) {
    FileOperations ops;
    m_tracker.extract(assistantCalling({{"read", Json{{"path", "a"}}},
                                        {"write", Json{{"path", "b"}}},
                                        {"edit", Json{{"path", "c"}}},
                                        {"bash", Json{{"path", "z"}}},
                                        {"read", Json{{"other", 1}}}}),
                      ops);
    EXPECT_EQ(ops.read, std::set<std::string>{"a"});
    EXPECT_EQ(ops.written, std::set<std::string>{"b"});
    EXPECT_EQ(ops.edited, std::set<std::string>{"c"});
}

TEST_F(FileOperationTrackerTest, ExtractsNestedCallsFromToolResults) {
    ToolResultMessage result;
    NestedToolCalls nested;
    NestedToolCallRecord record;
    record.name = "edit";
    record.arguments = Json{{"path", "n"}};
    nested.calls.push_back(record);
    result.nestedCalls = nested;
    FileOperations ops;
    m_tracker.extract(AgentMessage(result), ops);
    EXPECT_EQ(ops.edited, std::set<std::string>{"n"});
}

TEST_F(FileOperationTrackerTest, ListsSeparateReadOnlyFromModified) {
    FileOperations ops;
    ops.read = {"b", "a", "m"};
    ops.written = {"m"};
    ops.edited = {"z"};
    const auto lists = m_tracker.lists(ops);
    EXPECT_EQ(lists.readFiles, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(lists.modifiedFiles, (std::vector<std::string>{"m", "z"}));
}

TEST_F(FileOperationTrackerTest, SeedUsesDetailsFields) {
    FileOperations ops;
    m_tracker.seed(Json{{"readFiles", {"r"}}, {"modifiedFiles", {"m"}}}, ops);
    m_tracker.seed(Json("junk"), ops);
    EXPECT_EQ(ops.read, std::set<std::string>{"r"});
    EXPECT_EQ(ops.edited, std::set<std::string>{"m"});
}

TEST_F(FileOperationTrackerTest, FormatsTags) {
    EXPECT_EQ(m_tracker.format(FileLists{}), "");
    EXPECT_EQ(m_tracker.format(FileLists{{"a", "b"}, {"c"}}),
              "\n\n<read-files>\na\nb\n</read-files>\n\n<modified-files>\nc\n</modified-files>");
    EXPECT_EQ(m_tracker.format(FileLists{{}, {"c"}}), "\n\n<modified-files>\nc\n</modified-files>");
}
