export module pi.types.text_edit;

import std;

/** One requested replacement; oldText must match a unique region of the original file. */
export struct TextEdit {
    std::string oldText;
    std::string newText;
};
