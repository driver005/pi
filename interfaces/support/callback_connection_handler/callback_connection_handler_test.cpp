#include <gtest/gtest.h>

import std;
import pi.support.callback_connection_handler;

TEST(CallbackConnectionHandlerTest, ForwardsEachEventToItsCallback) {
    std::string log;
    ClientTransportHandlers callbacks;
    callbacks.onData = [&log](std::string_view chunk) { log += "data:" + std::string(chunk) + ";"; };
    callbacks.onClose = [&log] { log += "close;"; };
    callbacks.onError = [&log](const Error& error) { log += "error:" + error.code + ";"; };
    CallbackConnectionHandler handler(callbacks);
    handler.onData("abc");
    handler.onError(Error{"socket_error", "x"});
    handler.onClose();
    EXPECT_EQ(log, "data:abc;error:socket_error;close;");
}

TEST(CallbackConnectionHandlerTest, MissingCallbacksAreIgnored) {
    CallbackConnectionHandler handler{ClientTransportHandlers{}};
    handler.onData("x");
    handler.onError(Error{"e", "m"});
    handler.onClose();
}
