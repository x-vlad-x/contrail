#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Contrail {

class ChatView;

// Reactions, as in the phone apps.

// A reaction as text: the emoji itself, "custom:ID" for a custom emoji (Telegram Premium), "paid" for the Stars.
[[nodiscard]] std::string ReactionKey(const Td::api::ReactionType *type);
[[nodiscard]] Td::api::object_ptr<Td::api::ReactionType> ReactionFromKey(const std::string &key);
// The reactions under a message, added up.
[[nodiscard]] int ReactionsCount(const Td::api::message &message);

// The chips under a message: the emoji with the count, or with the avatars of the few who reacted; the user's
// own lit. A tap adds or takes back that reaction, a long press (in groups) shows who reacted. Null without
// reactions.
[[nodiscard]] GtkWidget *MakeReactionsBar(ChatView &chat, const Td::api::message &message);
// What the bar shows, apart from its numbers: the bar is made again only when this changes. A reaction coming or
// going in a busy channel changes only the numbers, and those are set in place (UpdateReactionsBar).
[[nodiscard]] std::string ReactionsLayoutKey(const ChatView &chat, const Td::api::message &message);
[[nodiscard]] std::string ReactionsCountKey(const Td::api::message &message);
void UpdateReactionsBar(GtkWidget *bar, const Td::api::message &message);

// The reactions at the top of a message's menu: the first ones possible there, then a button with all of them.
// `chosen` is called after one is tapped (to close the menu). Null when the message can't get any.
[[nodiscard]] GtkWidget *MakeReactionsStrip(ChatView &chat, const Td::api::message &message,
                                            std::shared_ptr<Td::api::availableReactions> available,
                                            std::function<void()> chosen);
// All the reactions the message can get.
void ShowReactionsChooser(ChatView &chat, std::int64_t message, std::shared_ptr<Td::api::availableReactions> available);

// Who reacted (groups; channels and private chats don't tell), with one reaction or all of them.
[[nodiscard]] bool CanShowWhoReacted(const ChatView &chat, const Td::api::message &message);
void ShowWhoReacted(ChatView &chat, const Td::api::message &message, const std::string &filter = {});

// Adds the reaction, or takes it back when it is the user's already. Paid ones aren't sent from here.
void ToggleReaction(ChatView &chat, const Td::api::message &message, const std::string &key);
// Double tap: the user's quick reaction (Telegram's default one), if the message can get it; else the first one
// it can.
void ToggleQuickReaction(ChatView &chat, const Td::api::message &message);

// For the debug state: the custom emoji pictures known and the textures kept.
[[nodiscard]] std::string CustomReactionsState();

}  // namespace Contrail
