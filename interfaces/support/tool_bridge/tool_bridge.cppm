export module pi.support.tool_bridge;

import std;
export import pi.support.message_codec;
export import pi.support.tool_set_cache;
export import pi.types.extension;

/**
 * Offers agent tools (ITool) to the durable harness as an extension. Each tool becomes a registration with the tool's own
 * declaration, argument repair and execution mode; its execution runs the tool of the call's directory (the conversation's
 * environment) with the call's id and abort signal and maps the result: content blocks, details, error flag, `terminate`.
 * Running output is the tool's final content (partial updates are not forwarded). Counterpart of the durable coding tools
 * of packages/durable/src/tools, which this reuses instead of porting a second set.
 */
export class ToolBridge {
public:
    ToolBridge(std::shared_ptr<ToolSetCache> tools, std::string defaultCwd)
        : m_tools(std::move(tools)),
          m_defaultCwd(std::move(defaultCwd)) {}

    /** The extension `name` offering every tool of the default directory's set. */
    Extension extension(const std::string& name) const {
        Extension extension;
        extension.name = name;
        for (const std::shared_ptr<ITool>& tool : m_tools->tools(m_defaultCwd)) {
            extension.tools.push_back(registration(*tool));
        }
        return extension;
    }

private:
    ToolRegistration registration(const ITool& tool) const {
        const std::string name = tool.definition().name;
        ToolRegistration registration;
        registration.name = name;
        registration.description = tool.definition().description;
        registration.parameters = tool.definition().parameters;
        if (const auto mode = tool.executionMode()) {
            registration.executionMode = *mode == ToolExecutionMode::Sequential ? "sequential" : "parallel";
        }
        registration.prepareArguments = [tools = m_tools, cwd = m_defaultCwd, name](const Json& args) -> Result<Json> {
            const std::shared_ptr<ITool> found = tools->find(cwd, name);
            return found ? found->prepareArguments(args) : args;
        };
        if (name == "bash") {
            registration.outputLimits = OutputLimits{50 * 1024, 2000, "tail"};
        }
        registration.execute = [self = *this, name](const Json& args, IToolExecutionApi& api) { return self.run(name, args, api); };
        return registration;
    }

    Result<ToolExecutionResult> run(const std::string& name, const Json& args, IToolExecutionApi& api) const {
        const std::shared_ptr<IExecutionEnv> env = api.env();
        const std::string cwd = env ? env->cwd() : m_defaultCwd;
        const std::shared_ptr<ITool> tool = m_tools->find(cwd, name);
        if (!tool) {
            return std::unexpected(Error{"tool_unavailable", "Tool " + name + " is not available in " + cwd});
        }
        // A non-owning handle: the invocation owns its signal and outlives the call.
        const std::shared_ptr<AbortSignal> signal(std::shared_ptr<void>(), &api.signal());
        auto result = tool->execute(api.callId(), args, signal, [](const AgentToolResult&) {});
        if (!result) {
            return std::unexpected(result.error());
        }
        const MessageCodec codec;
        ToolExecutionResult out;
        Json content = Json::array();
        for (const UserContentBlock& block : result->content) {
            content.push_back(codec.toJson(block));
        }
        out.content = std::move(content);
        if (result->isError) {
            out.isError = true;
        }
        if (!result->details.is_null()) {
            out.details = result->details;
        }
        if (result->terminate) {
            out.control = Json::object({{"terminate", true}});
        }
        return out;
    }

    std::shared_ptr<ToolSetCache> m_tools;
    std::string m_defaultCwd;
};
