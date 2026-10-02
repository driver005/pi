export module pi.types.stop_reason;

import std;

/** Why a response ended. JSON names: pending stop length toolUse error aborted deferred. */
export enum class StopReason { Pending, Stop, Length, ToolUse, Error, Aborted, Deferred };
