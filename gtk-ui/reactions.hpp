#pragma once

#include "app.hpp"

#include "base/basic_types.h"
#include "data/data_message_reaction_id.h"

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Contrail {

class MainWindow;

// Reactions, as in the phone apps.

// The chips under a message: the emoji with the count, or with the avatars of the few who reacted; the user's
// own lit. A tap adds or takes back that reaction, a long press (in groups) shows who reacted. Null without
// reactions.
[[nodiscard]] GtkWidget *MakeReactionsBar(not_null<HistoryItem *> item, not_null<MainWindow *> window);

// The reactions at the top of a message's menu: the first ones possible in the chat, then a button with all of
// them. `chosen` is called after one is toggled (to close the menu). Null when the message can't get any.
[[nodiscard]] GtkWidget *MakeReactionsStrip(not_null<HistoryItem *> item, not_null<MainWindow *> window,
                                            Fn<void()> chosen);

// All the reactions the message can get.
void ShowReactionsChooser(not_null<MainWindow *> window, not_null<HistoryItem *> item);

// Who reacted (groups and chats; channels don't tell), with one reaction or all of them.
[[nodiscard]] bool CanShowWhoReacted(not_null<HistoryItem *> item);
void ShowWhoReacted(not_null<MainWindow *> window, not_null<HistoryItem *> item, Data::ReactionId filter = {});

// Double tap: the user's quick reaction (Telegram's "favorite" one), if the chat allows it.
void ToggleQuickReaction(not_null<HistoryItem *> item);
void ToggleReaction(not_null<HistoryItem *> item, const Data::ReactionId &id);

// The emoji of a reaction as text; a custom emoji shows the emoji it stands for (resolved later: `label` is
// updated). Under messages and in the menus a custom emoji is its picture.
void SetReactionLabel(GtkWidget *label, not_null<Main::Session *> session, const Data::ReactionId &id);
[[nodiscard]] int ReactionsCount(not_null<HistoryItem *> item);

}  // namespace Contrail
