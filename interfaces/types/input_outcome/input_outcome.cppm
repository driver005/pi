export module pi.types.input_outcome;

import std;
export import pi.types.image_content;

/** What the `input` plugin event decided: swallow the input, or continue with (possibly replaced) text and images. */
export struct InputOutcome {
    bool handled = false;
    std::string text;
    std::vector<ImageContent> images;
};
