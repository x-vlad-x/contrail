#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <functional>
#include <string>

namespace Contrail {

class MainWindow;

// Chat info: picture, name, status, phone, username and link, bio or description, a group's members, with
// "Send message", calls, "Mute" and "Delete chat" / "Leave group".
void ShowProfile(MainWindow &window, Td::ChatId chat);
void ShowAbout(MainWindow &window);
// The contacts, as a page for the sidebar: a search, then a tap opens the chat.
[[nodiscard]] AdwNavigationPage *MakeContactsPage(MainWindow &window);

// What the card's call buttons do: they are shown only once the calls have set it.
void SetProfileCallHandler(std::function<void(Td::UserId user, bool video)> handler);

// "online", "last seen today at 12:34".
[[nodiscard]] std::string UserStatusText(const Td::User &user);

// The debug commands profile-… and contacts-… (debug.cpp). False when it isn't one of them.
bool ProfileDebugCommand(MainWindow &window, const std::string &name, const std::string &rest);

}  // namespace Contrail
