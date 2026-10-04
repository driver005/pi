export module pi.types.jsonl_file;

import std;
export import pi.types.jsonl_line;

/** The complete lines of one JSONL file. */
export struct JsonlFile {
    std::string path;
    std::vector<JsonlLine> lines;
};
