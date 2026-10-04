export module pi.types.error;

import std;

/** Failure value carried by std::expected across module boundaries. */
export struct Error {
    std::string code;
    std::string message;
};
