#pragma once

#include "app.hpp"

#include "data/data_types.h"

namespace Contrail {

class MainWindow;

// "Forward", as the phone apps do it: the chats to choose from (in the chat list's order, Saved Messages first,
// with a search; a group with topics asks for the topic), then the chosen chat opens with the messages waiting
// above its message field, to go with a comment or alone, with or without the senders' names. They wait in the
// chat's forward draft (History::setForwardDraft), as in Telegram Desktop: anything sent there takes them along.
void ShowForwardPicker(not_null<MainWindow *> window, MessageIdsList ids);

}  // namespace Contrail
