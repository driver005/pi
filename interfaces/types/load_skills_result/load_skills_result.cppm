export module pi.types.load_skills_result;

import std;
export import pi.types.resource_diagnostic;
export import pi.types.skill;

export struct LoadSkillsResult {
    std::vector<Skill> skills;
    std::vector<ResourceDiagnostic> diagnostics;
};
