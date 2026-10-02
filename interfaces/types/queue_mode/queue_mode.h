#pragma once

/** All: drain every queued message at a drain point. OneAtATime: drain only the oldest. */
enum class QueueMode { All, OneAtATime };
