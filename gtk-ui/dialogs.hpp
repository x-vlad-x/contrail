#pragma once

#include "app.hpp"

class PeerData;
class PhotoData;

namespace Main {
class Session;
} // namespace Main

namespace Contrail {

class MainWindow;

// Chat info: picture, name, status, phone, username, bio, with "Send message" and "Mute".
void ShowProfile(not_null<MainWindow *> window, not_null<PeerData *> peer);
// A full-screen photo with saving.
void ShowPhoto(not_null<MainWindow *> window, not_null<PhotoData *> photo, FullMsgId origin);
void ShowAbout(not_null<MainWindow *> window);
// "Delete chat" / "Leave group" / "Leave channel" with a confirmation.
void ConfirmDeleteChat(not_null<MainWindow *> window, not_null<PeerData *> peer);
// The contacts, as a page for the sidebar.
AdwNavigationPage *MakeContactsPage(not_null<Main::Session *> session, not_null<MainWindow *> window);

}  // namespace Contrail
