export module pi.platform.i_id_generator;

import std;

/** Produces unique, time-sortable identifiers (UUIDv7 in production). Thread-safe. */
export class IIdGenerator {
public:
    virtual ~IIdGenerator() = default;

    virtual std::string next() = 0;
};
