export module pi.types.queue_mode;

import std;

/** All: drain every queued message at a drain point. OneAtATime: drain only the oldest. */
export enum class QueueMode { All, OneAtATime };
