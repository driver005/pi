export module pi.types.client_attachment;

import std;
export import pi.server.i_service_attachment;

/** One client's live attachment to a hosted session, with the service calls running through it. */
export struct ClientAttachment {
    std::string id;
    std::uint64_t client = 0;
    std::string sessionId;
    /** Identifies the HostedSession instance this attached to (ids are reused after a restart). */
    std::uint64_t epoch = 0;
    std::unique_ptr<IServiceAttachment> lease;

    std::mutex mutex;
    std::condition_variable changed;
    int operations = 0;
    bool releasing = false;
    bool released = false;
};
