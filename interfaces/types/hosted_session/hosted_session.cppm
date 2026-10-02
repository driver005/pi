export module pi.types.hosted_session;

import std;
export import pi.server.i_routed_session_handle;
export import pi.types.client_attachment;

/** One opened session and the attachments connected to it. */
export struct HostedSession {
    std::string id;
    std::uint64_t epoch = 0;
    std::shared_ptr<IRoutedSessionHandle> handle;
    std::set<std::shared_ptr<ClientAttachment>> attachments;
};
