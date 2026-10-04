export module pi.support.skill_prompt_formatter;

import std;
export import pi.types.skill;

/** The <available_skills> block of the system prompt (Agent Skills format). */
export class SkillPromptFormatter {
public:
    /** Empty when no skill is visible. fileReadTool is "read" or "bash". */
    std::string format(const std::vector<Skill>& skills, const std::string& fileReadTool) const {
        std::vector<const Skill*> visible;
        for (const auto& skill : skills) {
            if (!skill.disableModelInvocation) {
                visible.push_back(&skill);
            }
        }
        if (visible.empty()) {
            return "";
        }
        std::string out = "\n\nThe following skills provide specialized instructions for specific tasks.\n";
        out += fileReadTool == "read"
                   ? "Use the read tool to load a skill's file when the task matches its description.\n"
                   : "Use bash to load a skill's file when the task matches its description.\n";
        out += "When a skill file references a relative path, resolve it against the skill directory (parent of "
               "SKILL.md / dirname of the path) and use that absolute path in tool commands.\n\n<available_skills>";
        for (const Skill* skill : visible) {
            out += "\n  <skill>\n    <name>" + escapeXml(skill->name) + "</name>\n    <description>" +
                   escapeXml(skill->description) + "</description>\n    <location>" + escapeXml(skill->filePath) +
                   "</location>\n  </skill>";
        }
        out += "\n</available_skills>";
        return out;
    }

    std::string escapeXml(const std::string& text) const {
        std::string out;
        for (const char c : text) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&apos;"; break;
                default: out.push_back(c);
            }
        }
        return out;
    }
};
