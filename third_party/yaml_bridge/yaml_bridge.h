#pragma once

#include <string>

/**
 * Parses YAML text and renders it as JSON text (scalars typed: bool, null, integer, double,
 * string). Returns false and fills *error for malformed input.
 *
 * yaml-cpp reports errors by throwing, so this bridge is built WITH exceptions and catches
 * everything itself. C++ modules built with -fno-exceptions (all pi modules) cannot import a
 * module compiled with exceptions, which is why this is an ordinary translation unit.
 */
bool yamlToJson(const std::string& yaml, std::string* json, std::string* error);
