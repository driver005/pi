export module pi.support.bash_command_executor;

import std;
export import pi.platform.i_crypto;
export import pi.platform.i_environment;
export import pi.platform.i_file_system;
export import pi.platform.i_process_runner;
export import pi.support.abort_signal;
export import pi.support.output_accumulator;
export import pi.support.output_sanitizer;
export import pi.support.shell_resolver;
export import pi.types.bash_result;
export import pi.types.result;

/**
 * Runs a command the user typed (not a tool call): through the shell in its own process group,
 * streaming sanitized chunks, keeping the output tail and spilling oversized output to a file.
 * Port of executeBashWithOperations in core/bash-executor.ts.
 */
export class BashCommandExecutor {
public:
    BashCommandExecutor(IProcessRunner& runner, IFileSystem& files, ICrypto& crypto, IEnvironment& environment)
        : m_runner(runner),
          m_files(files),
          m_crypto(crypto),
          m_shell(files, environment) {}

    /** Error only when the shell could not be started; cancellation is a normal result. */
    Result<BashResult> execute(const std::string& command, const std::string& cwd, const std::string& shellPath, const std::function<void(const std::string&)>& onChunk, const std::shared_ptr<AbortSignal>& signal) {
        OutputAccumulator output(m_files, m_crypto, "pi-bash");
        OutputSanitizer sanitizer;
        ProcessRequest request;
        request.command = m_shell.resolve(shellPath);
        request.args = {"-c", command};
        request.cwd = cwd;
        request.signal = signal;
        request.captureOutput = false;
        request.onOutput = [&](std::string_view chunk) {
            const std::string text = sanitizer.feed(chunk);
            if (text.empty()) {
                return;
            }
            output.append(text);
            if (onChunk) {
                onChunk(text);
            }
        };
        const auto outcome = m_runner.run(request);
        output.finish();
        const bool cancelled = (signal != nullptr && signal->aborted()) || (outcome && outcome->aborted);
        if (!outcome && !cancelled) {
            return std::unexpected(outcome.error());
        }
        const OutputSnapshot snapshot = output.snapshot(true);
        BashResult result;
        result.output = snapshot.content;
        result.truncated = snapshot.truncation.truncated;
        result.fullOutputPath = snapshot.fullOutputPath;
        result.cancelled = cancelled;
        if (!cancelled && outcome && outcome->exitCode >= 0) {
            result.exitCode = outcome->exitCode;
        }
        return result;
    }

private:
    IProcessRunner& m_runner;
    IFileSystem& m_files;
    ICrypto& m_crypto;
    ShellResolver m_shell;
};
