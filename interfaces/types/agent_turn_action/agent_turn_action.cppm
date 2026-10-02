export module pi.types.agent_turn_action;

import std;

/** finishTurn decision: Continue guarantees one more provider request, End stops the run. */
export enum class AgentTurnAction { Continue, End };
