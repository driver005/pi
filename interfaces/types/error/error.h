#pragma once

#include <string>

/** Failure value carried by std::expected across module boundaries. */
struct Error {
    std::string code;
    std::string message;
};
