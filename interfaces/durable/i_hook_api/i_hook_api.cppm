module;

#include <cstdint>

export module pi.durable.i_hook_api;

import std;
export import pi.types.doc_address_args;
export import pi.types.doc_definition;
export import pi.types.json;
export import pi.types.result;

/** What a hook or task may use: committed document reads, and the asking task's memos (shared by hooks and the task). */
export class IHookApi {
public:
    virtual ~IHookApi() = default;

    virtual std::int64_t taskId() const = 0;
    virtual std::int64_t conversationId() const = 0;
    /** The durable memo `name`, if set. */
    virtual Result<std::optional<Json>> memo(const std::string& name) = 0;
    /** Stores `candidate` unless a memo exists; returns the durable winner. */
    virtual Result<Json> memo(const std::string& name, const Json& candidate) = 0;
    /** The committed value of a document; nothing when it does not exist. */
    virtual Result<std::optional<Json>> snapshot(const DocDefinition& definition, const DocAddressArgs& args) = 0;
    /** A conversation document's value as of one visible entry's commit. */
    virtual Result<std::optional<Json>> snapshotAsOf(const DocDefinition& definition, const DocAddressArgs& args,
                                                     std::int64_t entryId) = 0;
};
