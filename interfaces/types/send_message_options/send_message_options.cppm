export module pi.types.send_message_options;

import std;

/** How a custom message reaches the conversation while a run may be active. */
export enum class DeliverAs { Default, Steer, FollowUp, NextTurn };

export struct SendMessageOptions {
    /** Start a turn for the message; unset means "yes while streaming, no otherwise". */
    std::optional<bool> triggerTurn;
    DeliverAs deliverAs = DeliverAs::Default;
};
