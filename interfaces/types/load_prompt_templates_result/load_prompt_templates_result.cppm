export module pi.types.load_prompt_templates_result;

import std;
export import pi.types.prompt_template;
export import pi.types.resource_diagnostic;

export struct LoadPromptTemplatesResult {
    std::vector<PromptTemplate> templates;
    std::vector<ResourceDiagnostic> diagnostics;
};
