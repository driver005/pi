export module pi.types.connection_stage;

/** Where one server connection is in its life. */
export enum class ConnectionStage { AwaitingHello, Handshaking, Ready, Closing, Closed };
