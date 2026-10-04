export module pi.types.tool_registration;

import std;
export import pi.durable.i_tool_execution_api;
export import pi.types.json;
export import pi.types.output_limits;
export import pi.types.result;
export import pi.types.tool_execution_result;

/**
 * An executable tool offered to the model. The declaration (`name`, `description`, `parameters`) is what enters
 * the transcript; the harness validates the arguments against `parameters` before `execute`.
 */
export struct ToolRegistration {
    std::string name;
    std::string description;
    /** JSON schema of the arguments. */
    Json parameters = Json::object();
    /** Whether an interrupted execution may rerun on recovery: "safe" or "unsafe" (default). */
    std::string replay = "unsafe";
    /** "parallel" or "sequential"; absent follows the settings. */
    std::optional<std::string> executionMode;
    /** Repairs arguments models commonly get wrong before validation; must be pure. */
    std::function<Result<Json>(const Json& args)> prepareArguments;
    std::optional<OutputLimits> outputLimits;
    std::function<Result<ToolExecutionResult>(const Json& args, IToolExecutionApi& api)> execute;
};
