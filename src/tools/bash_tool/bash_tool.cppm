module;
#include <nlohmann/json.hpp>

export module pi.tools.bash_tool;

import std;
import pi.platform.i_crypto;
import pi.platform.i_environment;
import pi.platform.i_file_system;
import pi.platform.i_process_runner;
import pi.support.text_truncator;
import pi.support.tool_result_factory;
import pi.support.output_accumulator;
import pi.support.shell_resolver;
export import pi.tool.i_tool;

/**
 * The `bash` tool (port of core/tools/bash.ts): runs a command through the shell in its own
 * process group, streams output, keeps the tail (50KB / 2000 lines) and spills the rest to a
 * temp file. Non-zero exits, aborts and timeouts are reported to the model as errors.
 */
export class BashTool : public ITool {
public:
    static constexpr std::int64_t kStructuredOutputMaxBytes = 1024 * 1024;
    static constexpr std::int64_t kUpdateThrottleMs = 100;
    static constexpr double kMaxTimeoutSeconds = 2147483.647;

    BashTool(IProcessRunner& runner, IFileSystem& fileSystem, ICrypto& crypto, IEnvironment& environment,
             std::string cwd, std::string shellPath = "", std::string commandPrefix = "")
        : m_runner(runner),
          m_fileSystem(fileSystem),
          m_crypto(crypto),
          m_shell(fileSystem, environment),
          m_cwd(std::move(cwd)),
          m_shellPath(std::move(shellPath)),
          m_commandPrefix(std::move(commandPrefix)) {
        m_definition.name = "bash";
        m_definition.description =
            "Execute a bash command in the current working directory. Returns stdout and stderr. Output is "
            "truncated to last 2000 lines or 50KB (whichever is hit first). If truncated, full output is saved "
            "to a temp file. Optionally provide a timeout in seconds.";
        m_definition.parameters = Json::parse(R"json({
            "type":"object",
            "properties":{
                "command":{"type":"string","description":"Shell command to execute"},
                "timeout":{"type":"number","description":"Timeout in seconds (optional, no default timeout)"}
            },
            "required":["command"]})json");
        m_definition.constrainedSampling = Json::parse(R"json({"type":"json_schema","strict":"prefer"})json");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return "bash";
    }
    std::optional<ToolExecutionMode> executionMode() const override {
        return std::nullopt;
    }
    Json prepareArguments(const Json& arguments) const override {
        return arguments;
    }

    Result<AgentToolResult> execute(const std::string&, const Json& params,
                                    const std::shared_ptr<AbortSignal>& signal,
                                    const ToolUpdateCallback& onUpdate) override {
        std::chrono::milliseconds timeout{0};
        if (params.contains("timeout") && params["timeout"].is_number()) {
            const double seconds = params["timeout"].get<double>();
            if (!std::isfinite(seconds) || seconds <= 0) {
                return std::unexpected(Error{"invalid_timeout", "Invalid timeout: must be a finite number of seconds"});
            }
            if (seconds > kMaxTimeoutSeconds) {
                return std::unexpected(Error{"invalid_timeout", "Invalid timeout: maximum is 2147483.647 seconds"});
            }
            timeout = std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000));
        }
        if (signal != nullptr && signal->aborted()) {
            return std::unexpected(Error{"aborted", "Operation aborted"});
        }
        if (!m_fileSystem.exists(m_cwd)) {
            return std::unexpected(Error{
                "cwd", "Working directory does not exist: " + m_cwd + "\nCannot execute bash commands."});
        }
        const std::string command = params["command"].get<std::string>();
        return runCommand(m_commandPrefix.empty() ? command : m_commandPrefix + "\n" + command, params, timeout,
                          signal, onUpdate);
    }

private:
    std::string resolveShell() const {
        return m_shell.resolve(m_shellPath);
    }

    std::string appendStatus(const std::string& text, const std::string& status) const {
        return (text.empty() ? "" : text + "\n\n") + status;
    }

    std::string formatOutput(const OutputSnapshot& snapshot, const OutputAccumulator& output, Json& details,
                             const std::string& emptyText) const {
        const TruncationResult& truncation = snapshot.truncation;
        std::string text = snapshot.content.empty() ? emptyText : snapshot.content;
        if (!truncation.truncated) {
            return text;
        }
        details = Json{{"truncation", m_results.truncationJson(truncation)}};
        const std::string full = snapshot.fullOutputPath.value_or("");
        if (snapshot.fullOutputPath.has_value()) {
            details["fullOutputPath"] = full;
        }
        const std::int64_t startLine = truncation.totalLines - truncation.outputLines + 1;
        const std::int64_t endLine = truncation.totalLines;
        if (truncation.lastLinePartial) {
            text += "\n\n[Showing last " + m_truncator.formatSize(truncation.outputBytes) + " of line " +
                    std::to_string(endLine) + " (line is " + m_truncator.formatSize(output.lastLineBytes()) +
                    "). Full output: " + full + "]";
        } else if (truncation.truncatedBy == "lines") {
            text += "\n\n[Showing lines " + std::to_string(startLine) + "-" + std::to_string(endLine) + " of " +
                    std::to_string(truncation.totalLines) + ". Full output: " + full + "]";
        } else {
            text += "\n\n[Showing lines " + std::to_string(startLine) + "-" + std::to_string(endLine) + " of " +
                    std::to_string(truncation.totalLines) + " (" +
                    m_truncator.formatSize(TextTruncator::kDefaultMaxBytes) + " limit). Full output: " + full + "]";
        }
        return text;
    }

    Result<AgentToolResult> runCommand(const std::string& command, const Json& params,
                                       std::chrono::milliseconds timeout,
                                       const std::shared_ptr<AbortSignal>& signal,
                                       const ToolUpdateCallback& onUpdate) {
        OutputAccumulator output(m_fileSystem, m_crypto, "pi-bash");
        const auto started = std::chrono::steady_clock::now();
        auto lastUpdate = started - std::chrono::milliseconds(kUpdateThrottleMs);
        if (onUpdate) {
            onUpdate(AgentToolResult{});
        }
        ProcessRequest request;
        request.command = resolveShell();
        request.args = {"-c", command};
        request.cwd = m_cwd;
        request.timeout = timeout;
        request.signal = signal;
        request.captureOutput = false;
        request.onOutput = [&](std::string_view chunk) {
            output.append(chunk);
            const auto now = std::chrono::steady_clock::now();
            if (onUpdate && now - lastUpdate >= std::chrono::milliseconds(kUpdateThrottleMs)) {
                lastUpdate = now;
                const OutputSnapshot snapshot = output.snapshot(true);
                Json details;
                const std::string text = formatOutput(snapshot, output, details, "");
                AgentToolResult partial = m_results.text(snapshot.content, details);
                onUpdate(partial);
                (void)text;
            }
        };
        auto outcome = m_runner.run(request);
        output.finish();
        if (!outcome.has_value()) {
            return std::unexpected(outcome.error());
        }
        const OutputSnapshot snapshot = output.snapshot(true);
        Json details;
        const std::string outputText = formatOutput(snapshot, output, details, "(no output)");
        if (outcome->aborted || outcome->timedOut) {
            Json ignored;
            const std::string partialText = formatOutput(snapshot, output, ignored, "");
            const std::string status = outcome->aborted ? "Command aborted"
                                                        : "Command timed out after " + params["timeout"].dump() + " seconds";
            return std::unexpected(Error{outcome->aborted ? "aborted" : "timeout", appendStatus(partialText, status)});
        }
        const int exitCode = outcome->exitCode >= 0 ? outcome->exitCode : (outcome->termSignal > 0 ? 128 + outcome->termSignal : 1);
        const double wall = std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * 10) / 10;
        const FullOutput full = output.readFullOutput(kStructuredOutputMaxBytes);
        Json structured = {{"output", full.content}, {"truncated", full.truncated}};
        if (full.truncated && snapshot.fullOutputPath.has_value()) {
            structured["full_output_path"] = *snapshot.fullOutputPath;
        }
        structured["exit_code"] = exitCode;
        structured["wall_time_seconds"] = wall;
        AgentToolResult result = m_results.text(exitCode == 0 ? outputText
                                                              : appendStatus(outputText, "Command exited with code " + std::to_string(exitCode)),
                                                details);
        result.structuredContent = std::move(structured);
        result.isError = exitCode != 0;
        return result;
    }

    IProcessRunner& m_runner;
    IFileSystem& m_fileSystem;
    ICrypto& m_crypto;
    ShellResolver m_shell;
    std::string m_cwd;
    std::string m_shellPath;
    std::string m_commandPrefix;
    TextTruncator m_truncator;
    ToolResultFactory m_results;
    Tool m_definition;
};
