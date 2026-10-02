module;

#include <nlohmann/json.hpp>

export module pi.rpc.rpc_server;

import std;
export import pi.platform.i_byte_input;
export import pi.platform.i_byte_output;
export import pi.platform.i_executor;
export import pi.provider.i_model_runtime;
export import pi.session.i_agent_session_runtime;
import pi.support.json_writer;
import pi.support.jsonl_line_reader;
import pi.support.rpc_command_router;

/**
 * `pi rpc`: reads JSONL commands from a byte stream, runs each on the executor (so a running
 * prompt does not block abort or state queries) and writes responses and session events as JSONL.
 * Ends when the input ends, after the running commands finish and the session is disposed.
 */
export class RpcServer {
public:
    RpcServer(IAgentSessionRuntime& runtime, IModelRuntime& models, IByteInput& input, IByteOutput& output,
              IExecutor& executor);

    /** Serves until end of input; returns the process exit code. */
    int run();

private:
    void write(const Json& json);
    void dispatch(const std::string& line);
    void startCommand(const Json& command);
    void waitForCommands();

    IAgentSessionRuntime& m_runtime;
    IByteInput& m_input;
    IByteOutput& m_output;
    IExecutor& m_executor;
    JsonWriter m_writer;
    RpcCommandRouter m_router;

    std::mutex m_mutex;
    std::condition_variable m_finished;
    int m_inFlight = 0;
};

RpcServer::RpcServer(IAgentSessionRuntime& runtime, IModelRuntime& models, IByteInput& input, IByteOutput& output,
                     IExecutor& executor)
    : m_runtime(runtime),
      m_input(input),
      m_output(output),
      m_executor(executor),
      m_router(runtime, models, [this](const Json& json) { write(json); }) {}

void RpcServer::write(const Json& json) {
    m_output.write(m_writer.compact(json) + "\n");
}

void RpcServer::startCommand(const Json& command) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_inFlight;
    }
    m_executor.submit([this, command] {
        m_router.handle(command);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            --m_inFlight;
        }
        m_finished.notify_all();
    });
}

void RpcServer::waitForCommands() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_finished.wait(lock, [this] { return m_inFlight == 0; });
}

void RpcServer::dispatch(const std::string& line) {
    const Json parsed = Json::parse(line, nullptr, false);
    if (parsed.is_discarded()) {
        write(m_router.parseError("invalid JSON"));
        return;
    }
    if (parsed.is_object() && parsed.value("type", "") == "extension_ui_response") {
        return;
    }
    startCommand(parsed);
}

int RpcServer::run() {
    m_router.attach();
    JsonlLineReader reader;
    while (const auto chunk = m_input.read()) {
        for (const auto& line : reader.feed(*chunk)) {
            dispatch(line);
        }
    }
    if (const auto tail = reader.finish()) {
        dispatch(*tail);
    }
    waitForCommands();
    m_router.detach();
    m_runtime.dispose();
    m_output.flush();
    return 0;
}
