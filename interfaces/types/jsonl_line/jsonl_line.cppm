export module pi.types.jsonl_line;

import std;
export import pi.types.json;

/** One parsed line of a JSONL file and the byte offset where it starts. */
export struct JsonlLine {
    Json value;
    std::size_t start = 0;
};
