#pragma once

#include <string>

/** Produces unique, time-sortable identifiers (UUIDv7 in production). Thread-safe. */
class IIdGenerator {
public:
    virtual ~IIdGenerator() = default;

    virtual std::string next() = 0;
};
