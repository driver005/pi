export module pi.tools.write_tool;

import std;
import pi.platform.i_file_system;
import pi.support.path_resolver;
import pi.support.tool_result_factory;
export import pi.tool.i_file_mutation_queue;
export import pi.tool.i_tool;

/** The `write` tool: creates or overwrites a file, creating parent directories. */
export class WriteTool : public ITool {
public:
    WriteTool(IFileSystem& fileSystem, IFileMutationQueue& queue, std::string cwd)
        : m_fileSystem(fileSystem), m_queue(queue), m_cwd(std::move(cwd)), m_paths(fileSystem.homeDirectory()) {
        m_definition.name = "write";
        m_definition.description =
            "Write content to a file. Creates the file if it doesn't exist, overwrites if it does. "
            "Automatically creates parent directories.";
        m_definition.parameters = Json::parse(R"json({
            "type":"object",
            "properties":{
                "path":{"type":"string","description":"Path to the file to write (relative or absolute)"},
                "content":{"type":"string","description":"Content to write to the file"}
            },
            "required":["path","content"]})json");
        m_definition.constrainedSampling = Json::parse(R"json({"type":"json_schema","strict":"prefer"})json");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return "write";
    }
    std::optional<ToolExecutionMode> executionMode() const override {
        return std::nullopt;
    }
    std::string promptSnippet() const override {
        return "Create or overwrite files";
    }
    std::vector<std::string> promptGuidelines() const override {
        return {
            "Use write only for new files or complete rewrites."};
    }
    Json prepareArguments(const Json& arguments) const override {
        return arguments;
    }

    Result<AgentToolResult> execute(const std::string&, const Json& params,
                                    const std::shared_ptr<AbortSignal>& signal,
                                    const ToolUpdateCallback&) override {
        const std::string path = params["path"].get<std::string>();
        const std::string content = params["content"].get<std::string>();
        const std::string absolute = m_paths.resolveToCwd(path, m_cwd);
        Result<AgentToolResult> outcome = std::unexpected(Error{"unreachable", ""});
        m_queue.run(absolute, [&] { outcome = writeLocked(absolute, path, content, signal); });
        return outcome;
    }

private:
    Result<AgentToolResult> writeLocked(const std::string& absolute, const std::string& path,
                                        const std::string& content,
                                        const std::shared_ptr<AbortSignal>& signal) {
        const auto aborted = [&] { return signal != nullptr && signal->aborted(); };
        const Error abortError{"aborted", "Operation aborted"};
        if (aborted()) {
            return std::unexpected(abortError);
        }
        if (auto made = m_fileSystem.createDirectories(std::filesystem::path(absolute).parent_path().string());
            !made.has_value()) {
            return std::unexpected(made.error());
        }
        if (aborted()) {
            return std::unexpected(abortError);
        }
        if (auto written = m_fileSystem.writeFile(absolute, content); !written.has_value()) {
            return std::unexpected(written.error());
        }
        if (aborted()) {
            return std::unexpected(abortError);
        }
        return m_results.text("Successfully wrote to " + path);
    }

    IFileSystem& m_fileSystem;
    IFileMutationQueue& m_queue;
    std::string m_cwd;
    PathResolver m_paths;
    ToolResultFactory m_results;
    Tool m_definition;
};
