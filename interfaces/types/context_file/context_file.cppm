export module pi.types.context_file;

import std;

/** An AGENTS.md / CLAUDE.md style instruction file included in the system prompt. */
export struct ContextFile {
    std::string path;
    std::string content;
};
