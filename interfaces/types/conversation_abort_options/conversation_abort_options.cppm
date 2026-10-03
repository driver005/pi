export module pi.types.conversation_abort_options;

/** Options of a conversation abort. */
export struct ConversationAbortOptions {
    /**
     * Cross background boundaries: mark every live task reached ignoring the background flag, withdraw the
     * queued inputs of every conversation reached, and wait until those tasks are terminal.
     */
    bool background = false;
};
