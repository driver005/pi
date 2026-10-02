#pragma once

#include <map>
#include <optional>
#include <string>

/** Process environment variables. Injected so tests never touch the real environment. */
class IEnvironment {
public:
    virtual ~IEnvironment() = default;

    virtual std::optional<std::string> get(const std::string& name) const = 0;
    virtual void set(const std::string& name, const std::string& value) = 0;
    virtual void unset(const std::string& name) = 0;
    virtual std::map<std::string, std::string> all() const = 0;
};
