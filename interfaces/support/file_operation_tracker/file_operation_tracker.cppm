export module pi.support.file_operation_tracker;

import std;
export import pi.types.agent_message;
export import pi.types.file_lists;
export import pi.types.file_operations;
export import pi.types.json;

/**
 * Tracks which files an agent read and modified, from tool calls in the transcript and from the
 * details of earlier summaries, and renders them as XML tags for a summary. Port of the file
 * operation helpers in compaction/utils.ts.
 */
export class FileOperationTracker {
public:
    /** Adds the paths of read/write/edit tool calls in an assistant message or nested results. */
    void extract(const AgentMessage& message, FileOperations& ops) const {
        if (const auto* result = std::get_if<ToolResultMessage>(&message)) {
            if (result->nestedCalls) {
                for (const auto& call : result->nestedCalls->calls) {
                    add(call.name, call.arguments, ops);
                }
            }
            return;
        }
        const auto* assistant = std::get_if<AssistantMessage>(&message);
        if (assistant == nullptr) {
            return;
        }
        for (const auto& block : assistant->content) {
            if (const auto* call = std::get_if<ToolCall>(&block)) {
                add(call->name, call->arguments, ops);
            }
        }
    }

    /** Seeds from an earlier summary's details {readFiles, modifiedFiles} (modified count as edited). */
    void seed(const Json& details, FileOperations& ops) const {
        if (!details.is_object()) {
            return;
        }
        if (const auto found = details.find("readFiles"); found != details.end()) {
            addAll(*found, ops.read);
        }
        if (const auto found = details.find("modifiedFiles"); found != details.end()) {
            addAll(*found, ops.edited);
        }
    }

    FileLists lists(const FileOperations& ops) const {
        std::set<std::string> modified = ops.edited;
        modified.insert(ops.written.begin(), ops.written.end());
        FileLists out;
        for (const auto& file : ops.read) {
            if (!modified.contains(file)) {
                out.readFiles.push_back(file);
            }
        }
        out.modifiedFiles.assign(modified.begin(), modified.end());
        return out;
    }

    /** "\n\n<read-files>...</read-files>\n\n<modified-files>...</modified-files>" or empty. */
    std::string format(const FileLists& lists) const {
        std::vector<std::string> sections;
        if (!lists.readFiles.empty()) {
            sections.push_back(section("read-files", lists.readFiles));
        }
        if (!lists.modifiedFiles.empty()) {
            sections.push_back(section("modified-files", lists.modifiedFiles));
        }
        std::string text;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            text += "\n\n" + sections[i];
        }
        return text;
    }

private:
    void add(const std::string& toolName, const Json& arguments, FileOperations& ops) const {
        if (!arguments.is_object()) {
            return;
        }
        const auto path = arguments.find("path");
        if (path == arguments.end() || !path->is_string() || path->get<std::string>().empty()) {
            return;
        }
        if (toolName == "read") {
            ops.read.insert(path->get<std::string>());
        } else if (toolName == "write") {
            ops.written.insert(path->get<std::string>());
        } else if (toolName == "edit") {
            ops.edited.insert(path->get<std::string>());
        }
    }

    void addAll(const Json& list, std::set<std::string>& into) const {
        if (!list.is_array()) {
            return;
        }
        for (const auto& item : list) {
            if (item.is_string()) {
                into.insert(item.get<std::string>());
            }
        }
    }

    std::string section(const std::string& tag, const std::vector<std::string>& files) const {
        std::string text = "<" + tag + ">\n";
        for (std::size_t i = 0; i < files.size(); ++i) {
            text += (i == 0 ? "" : "\n") + files[i];
        }
        return text + "\n</" + tag + ">";
    }
};
