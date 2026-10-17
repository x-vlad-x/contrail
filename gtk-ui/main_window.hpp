#pragma once

#include "app.hpp"

#include "base/weak_ptr.h"

#include <rpl/lifetime.h>

class History;
class PeerData;
class PhotoData;

namespace Main {
class Session;
class Account;
} // namespace Main

namespace Contrail {

class LoginView;
class ChatList;
class ChatView;
class SettingsView;
class CallView;
class TrayIcon;

class MainWindow final : public base::has_weak_ptr {
public:
  MainWindow();
  ~MainWindow();

  [[nodiscard]] GtkWidget *widget() const { return _window; }
  [[nodiscard]] bool isActive() const;
  void present();
  void showToast(const QString &text);

  // Opens a chat in the content pane (on a phone: pushes it over the chat list).
  void openHistory(not_null<History *> history, MsgId showAt = 0);
  void openPeer(not_null<PeerData *> peer);
  [[nodiscard]] History *currentHistory() const;
  void closeChat();
  void openSettings();
  void openArchive();
  void openContacts();
  void openProfile(not_null<PeerData *> peer);
  void openPhoto(not_null<PhotoData *> photo, FullMsgId origin);
  void showAbout();
  void confirmDeleteChat(not_null<PeerData *> peer);
  void searchChats(const QString &query);
  void sendBotCommand(const QString &command);
  void startCall(not_null<PeerData *> peer, bool video = false);
  void setCallStatus(const QString &text, bool active);
  // Closing the window either quits or leaves the app running, with an icon in the notification area.
  void updateBackgroundMode();
  [[nodiscard]] Main::Session *session() const { return _session; }

  // For the debug actions.
  void debugOpenChat(int index);
  void debugBack();
  void debugScroll(bool top);
  void debugCallPreview(bool incoming, bool video = false, const QString &name = u"Contrail"_q);
  void debugLocalCall(const QString &action);
  void debugCheckDraft();
  void debugLogin(const QString &step, const QString &value);
  void debugHistory(const QString &action);

private:
  void showAccount(Main::Account *account);
  void showSession(not_null<Main::Session *> session);
  void showLogin(not_null<Main::Account *> account);
  void clearSessionUi();
  void setupShortcuts();
  void followCoreWindow();

  GtkWidget *_window = nullptr;
  AdwToastOverlay *_toasts = nullptr;
  GtkStack *_stack = nullptr;
  AdwNavigationSplitView *_split = nullptr;
  AdwNavigationView *_sidebarNav = nullptr;
  AdwNavigationPage *_contentPage = nullptr;
  GtkWidget *_placeholder = nullptr;
  GtkWidget *_callBanner = nullptr;
  std::unique_ptr<CallView> _calls;
  std::unique_ptr<TrayIcon> _tray;

  Main::Session *_session = nullptr;
  std::unique_ptr<LoginView> _login;
  std::unique_ptr<ChatList> _chatList;
  std::unique_ptr<ChatList> _archive;
  std::unique_ptr<ChatView> _chat;
  std::unique_ptr<SettingsView> _settings;
  rpl::lifetime _sessionLifetime;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
