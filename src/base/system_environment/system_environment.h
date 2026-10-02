#pragma once

#include <map>
#include <optional>
#include <string>

#include "interfaces/platform/i_environment/i_environment.h"

/** IEnvironment over the real process environment (getenv/setenv). */
class SystemEnvironment : public IEnvironment {
public:
    std::optional<std::string> get(const std::string& name) const override;
    void set(const std::string& name, const std::string& value) override;
    void unset(const std::string& name) override;
    std::map<std::string, std::string> all() const override;
};
