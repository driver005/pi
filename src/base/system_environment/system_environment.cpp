#include "src/base/system_environment/system_environment.h"

#include <cstdlib>

extern char** environ;

std::optional<std::string> SystemEnvironment::get(const std::string& name) const {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

void SystemEnvironment::set(const std::string& name, const std::string& value) {
    setenv(name.c_str(), value.c_str(), 1);
}

void SystemEnvironment::unset(const std::string& name) {
    unsetenv(name.c_str());
}

std::map<std::string, std::string> SystemEnvironment::all() const {
    std::map<std::string, std::string> out;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string text(*entry);
        const std::size_t eq = text.find('=');
        if (eq != std::string::npos && eq > 0) {
            out[text.substr(0, eq)] = text.substr(eq + 1);
        }
    }
    return out;
}
