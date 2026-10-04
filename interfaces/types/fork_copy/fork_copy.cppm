export module pi.types.fork_copy;

export import pi.types.json;

/** One document copy to create with a forked conversation: the new record and its `{id, at}` source. */
export struct ForkCopy {
    Json record;
    Json source;
};
