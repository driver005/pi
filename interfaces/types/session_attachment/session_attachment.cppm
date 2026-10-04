export module pi.types.session_attachment;

import std;

/** One client's attachment to a hosted session, as announced to it in an `attachment` message. */
export struct SessionAttachment {
    std::string sessionId;
    std::string attachmentId;
};
