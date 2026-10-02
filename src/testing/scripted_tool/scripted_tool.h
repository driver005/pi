#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <string>

#include "interfaces/tool/i_tool/i_tool.h"

/** Configurable fake tool: returns fixed text after an optional delay, can request termination. */
class ScriptedTool : public ITool {
public:
    ScriptedTool(std::string name, std::string resultText, int delayMs = 0, bool terminate = false,
                 bool sequential = false)
        : m_resultText(std::move(resultText)),
          m_delayMs(delayMs),
          m_terminate(terminate),
          m_sequential(sequential) {
        m_definition.name = std::move(name);
        m_definition.description = "scripted";
        m_definition.parameters = Json::parse(R"({"type":"object","properties":{"arg":{"type":"string"}}})");
    }

    const Tool& definition() const override {
        return m_definition;
    }
    std::string label() const override {
        return m_definition.name;
    }
    std::optional<ToolExecutionMode> executionMode() const override {
        return m_sequential ? std::optional<ToolExecutionMode>(ToolExecutionMode::Sequential) : std::nullopt;
    }
    Json prepareArguments(const Json& arguments) const override {
        return arguments;
    }
    Result<AgentToolResult> execute(const std::string&, const Json&,
                                    const std::shared_ptr<AbortSignal>&,
                                    const ToolUpdateCallback&) override {
        ++m_executions;
        if (m_delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_delayMs));
        }
        AgentToolResult result;
        result.content.emplace_back(TextContent{m_resultText, std::nullopt});
        result.terminate = m_terminate;
        return result;
    }

    int executions() const {
        return m_executions.load();
    }

private:
    Tool m_definition;
    std::string m_resultText;
    int m_delayMs;
    bool m_terminate;
    bool m_sequential;
    std::atomic<int> m_executions{0};
};
