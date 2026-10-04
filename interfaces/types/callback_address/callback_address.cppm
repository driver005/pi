export module pi.types.callback_address;

import std;

/** Where the loopback callback server of a sign-in listens and the redirect URI it serves. */
export struct CallbackAddress {
    /** The address to listen on ("localhost" becomes 127.0.0.1). */
    std::string host;
    /** The host name of the redirect URI (IPv6 addresses bracketed). */
    std::string redirectHost;
    /** The configured port, 0 when any free port will do. */
    int port = 0;
    std::string path = "/callback";
    /** The exact redirect URI when the port is fixed by the configuration; empty otherwise. */
    std::string fixedRedirect;
};
