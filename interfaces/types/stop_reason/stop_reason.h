#pragma once

/** Why a response ended. JSON names: pending stop length toolUse error aborted deferred. */
enum class StopReason { Pending, Stop, Length, ToolUse, Error, Aborted, Deferred };
