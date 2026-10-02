module;
#include <nlohmann/json.hpp>

export module pi.tools.edit_tool;

import std;
import pi.platform.i_file_system;
import pi.support.edit_engine;
import pi.support.path_resolver;
import pi.support.text_diff;
import pi.support.tool_result_factory;
export import pi.tool.i_file_mutation_queue;
export import pi.tool.i_tool;

/**
 * The `edit` tool (port of core/tools/edit.ts): exact-text replacements, all matched against the
 * original file, with fuzzy fallback, BOM and CRLF preservation, and a diff in the details.
 */
export class EditTool : public ITool {
public:
    EditTool(IFileSystem& fileSystem, IFileMutationQueue& queue, std::string cwd)
        : m_fileSystem(fileSystem), m_queue(queue), m_cwd(std::move(cwd)), m_paths(fileSystem.homeDirectory()) {
        m_definition.name = "edit";
        m_definition.description =
            "Edit a single file using exact text replacement. Every edits[].oldText must match a unique, "
            "non-overlapping region of the original file. If two changes affect the same block or nearby "
            "lines, merge them into one edit instead of emitting overlapping edits. Do not include large "
            "unchanged regions just to connect distant changes.";
        m_definition.parameters = Json::parse(R"json({
            "type":"object",
            "properties":{
                "path":{"type":"string","description":"Path to the file to edit (relative or absolute)"},
                "edits":{"type":"array","description":"One or more targeted replacements. Each edit is matched against the original file, not incrementally. Do not include overlapping or nested edits. If two changes touch the same block or nearby lines, merge them into one edit instead.",
                    "items":{"type":"object","properties":{
                        "oldText":{"type":"string","description":"Exact text for one targeted replacement. It must be unique in the original file and must not overlap with any other edits[].oldText in the same call."},
                        "newText":{"type":"string","description":"Replacement text for this targeted edit."}},
                        "required":["oldText","newText"]}}
            },
            "required":["path","edits"]})json");
        m_definition.constrainedSampling = Json::parse(R"json({"type":"json_schema","strict":"prefer"})json");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return "edit";
    }
    std::optional<ToolExecutionMode> executionMode() const override {
        return std::nullopt;
    }

    /** Accepts `edits` as a JSON string or single object, and legacy top-level oldText/newText. */
    std::string promptSnippet() const override {
        return "Make precise file edits with exact text replacement, including multiple disjoint edits in one call";
    }
    std::vector<std::string> promptGuidelines() const override {
        return {
            "Use edit for precise changes (edits[].oldText must match exactly)",
            "When changing multiple separate locations in one file, use one edit call with multiple entries in edits[] instead of multiple edit calls",
            "Each edits[].oldText is matched against the original file, not after earlier edits are applied. Do not emit overlapping or nested edits. Merge nearby changes into one edit.",
            "Keep edits[].oldText as small as possible while still being unique in the file. Do not pad with large unchanged regions."};
    }
    Json prepareArguments(const Json& arguments) const override {
        if (!arguments.is_object()) {
            return arguments;
        }
        Json args = arguments;
        if (args.contains("edits") && args["edits"].is_string()) {
            const Json parsed = Json::parse(args["edits"].get<std::string>(), nullptr, false);
            if (parsed.is_array()) {
                args["edits"] = parsed;
            } else if (isSingleEdit(parsed)) {
                args["edits"] = Json::array({parsed});
            }
        } else if (args.contains("edits") && isSingleEdit(args["edits"])) {
            args["edits"] = Json::array({args["edits"]});
        }
        if (args.contains("oldText") && args["oldText"].is_string() && args.contains("newText") &&
            args["newText"].is_string()) {
            Json edits = args.contains("edits") && args["edits"].is_array() ? args["edits"] : Json::array();
            edits.push_back({{"oldText", args["oldText"]}, {"newText", args["newText"]}});
            args.erase("oldText");
            args.erase("newText");
            args["edits"] = edits;
        }
        return args;
    }

    Result<AgentToolResult> execute(const std::string&, const Json& params,
                                    const std::shared_ptr<AbortSignal>& signal,
                                    const ToolUpdateCallback&) override {
        if (!params.contains("edits") || !params["edits"].is_array() || params["edits"].empty()) {
            return std::unexpected(
                Error{"invalid_edit", "Edit tool input is invalid. edits must contain at least one replacement."});
        }
        const std::string path = params["path"].get<std::string>();
        std::vector<TextEdit> edits;
        for (const Json& entry : params["edits"]) {
            edits.push_back({entry.value("oldText", ""), entry.value("newText", "")});
        }
        const std::string absolute = m_paths.resolveToCwd(path, m_cwd);
        Result<AgentToolResult> outcome = std::unexpected(Error{"unreachable", ""});
        m_queue.run(absolute, [&] { outcome = editLocked(absolute, path, edits, signal); });
        return outcome;
    }

private:
    bool isSingleEdit(const Json& value) const {
        return value.is_object() && value.contains("oldText") && value["oldText"].is_string() &&
               value.contains("newText") && value["newText"].is_string();
    }

    Result<AgentToolResult> editLocked(const std::string& absolute, const std::string& path,
                                       const std::vector<TextEdit>& edits,
                                       const std::shared_ptr<AbortSignal>& signal) {
        const auto aborted = [&] { return signal != nullptr && signal->aborted(); };
        const Error abortError{"aborted", "Operation aborted"};
        if (aborted()) {
            return std::unexpected(abortError);
        }
        if (!m_fileSystem.isReadable(absolute) || !m_fileSystem.isWritable(absolute)) {
            const std::string code = m_fileSystem.exists(absolute) ? "EACCES" : "ENOENT";
            return std::unexpected(Error{code, "Could not edit file: " + path + ". Error code: " + code + "."});
        }
        auto raw = m_fileSystem.readFile(absolute);
        if (!raw.has_value()) {
            return std::unexpected(raw.error());
        }
        const auto [bom, content] = m_engine.splitBom(*raw);
        const std::string ending = m_engine.detectLineEnding(content);
        const std::string normalized = m_engine.normalizeToLF(content);
        auto applied = m_engine.applyEdits(normalized, edits, path);
        if (!applied.has_value()) {
            return std::unexpected(applied.error());
        }
        if (aborted()) {
            return std::unexpected(abortError);
        }
        if (auto written = m_fileSystem.writeFile(absolute, bom + m_engine.restoreLineEndings(applied->newContent, ending));
            !written.has_value()) {
            return std::unexpected(written.error());
        }
        const DisplayDiff diff = m_diff.displayDiff(applied->baseContent, applied->newContent);
        Json details = {{"diff", diff.diff},
                        {"patch", m_diff.unifiedPatch(path, applied->baseContent, applied->newContent)}};
        if (diff.firstChangedLine.has_value()) {
            details["firstChangedLine"] = *diff.firstChangedLine;
        }
        return m_results.text("Successfully replaced " + std::to_string(edits.size()) + " block(s) in " + path + ".",
                              std::move(details));
    }

    IFileSystem& m_fileSystem;
    IFileMutationQueue& m_queue;
    std::string m_cwd;
    PathResolver m_paths;
    EditEngine m_engine;
    TextDiff m_diff;
    ToolResultFactory m_results;
    Tool m_definition;
};
