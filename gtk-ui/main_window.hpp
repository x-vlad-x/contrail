#pragma once

#include "app.hpp"

#include "base/timer.h"
#include "base/weak_ptr.h"

#include <rpl/lifetime.h>

class History;
class PeerData;
class PhotoData;

namespace Main {
class Session;
class Account;
} // namespace Main

namespace Data {
class Forum;
class Thread;
} // namespace Data

namespace Contrail {

class LoginView;
class ChatList;
class ChatView;
class SettingsView;
class CallView;
class TrayIcon;

// The application's window. When it is closed and the app goes on in the background, the whole GTK
// interface is released (the chats, the lists, the dialogs, their textures and GPU memory) and built
// again from the session when the window is opened: only what is needed to come back to the same
// place is kept, apart from GTK objects and messages (UiState). The session, updates, notifications,
// links, downloads, calls and a voice message being recorded go on without it.
class MainWindow final : public base::has_weak_ptr {
public:
  MainWindow();
  ~MainWindow();

  // Null while the interface is released (the window is closed, the app runs in the background).
  [[nodiscard]] GtkWidget *widget() const { return _window; }
  [[nodiscard]] bool isActive() const;
  // Opens the window, building the interface again if it was released.
  void present();
  // Closing the window, the tray icon and launching the app again all come here and to present().
  void closeToBackground();
  void showToast(const QString &text);

  // Opens a chat in the content pane (on a phone: pushes it over the chat list). A group with
  // topics opens its list of topics in the sidebar instead; a topic opens as a chat of its own.
  void openHistory(not_null<History *> history, MsgId showAt = 0);
  void openThread(not_null<Data::Thread *> thread, MsgId showAt = 0);
  void openPeer(not_null<PeerData *> peer);
  void openForum(not_null<History *> history);
  void closeForum(Data::Forum *forum);
  [[nodiscard]] History *currentHistory() const;
  [[nodiscard]] Data::Thread *currentThread() const;
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
  void debugOpenTopic(int index);
  void debugBack();
  void debugScroll(bool top);
  void debugCallPreview(bool incoming, bool video = false, const QString &name = u"Contrail"_q);
  void debugLocalCall(const QString &action);
  void debugCheckDraft();
  void debugLogin(const QString &step, const QString &value);
  void debugHistory(const QString &action);
  void debugAttach(const QString &action);
  void debugState();

private:
  // Where the interface was, to come back there: a chat or a topic and the scroll position in it.
  struct UiState {
    PeerId peer = 0;
    MsgId topicRootId = 0;
    MsgId showAt = 0;       // A message to show (a notification, a link): wins over the scroll position.
    MsgId anchor = 0;       // The message at the top of the screen, none at the bottom of the chat.
    double anchorY = 0.;    // Where it was on the screen.
  };

  void buildWindow();
  void releaseWindow();
  void restore(UiState state);
  void showAccount(Main::Account *account);
  void showSession(not_null<Main::Session *> session);
  void showLogin(not_null<Main::Account *> account);
  void clearSessionUi();
  void setupShortcuts();
  void followCoreWindow();
  void openThreadAt(not_null<Data::Thread *> thread, MsgId showAt, MsgId anchor, double anchorY);
  void endRecordingOutside(Data::Thread *thread);

  GtkWidget *_window = nullptr;
  AdwToastOverlay *_toasts = nullptr;
  GtkStack *_stack = nullptr;
  AdwNavigationSplitView *_split = nullptr;
  AdwNavigationView *_sidebarNav = nullptr;
  AdwNavigationPage *_contentPage = nullptr;
  GtkWidget *_placeholder = nullptr;
  GtkWidget *_callBanner = nullptr;
  QString _callStatus;
  bool _callActive = false;
  std::unique_ptr<CallView> _calls;
  std::unique_ptr<TrayIcon> _tray;

  Main::Account *_account = nullptr;
  Main::Session *_session = nullptr;
  std::optional<UiState> _saved;  // Set while the interface is released.
  std::unique_ptr<LoginView> _login;
  std::unique_ptr<ChatList> _chatList;
  std::unique_ptr<ChatList> _archive;
  std::unique_ptr<ChatList> _forum;
  std::unique_ptr<ChatView> _chat;
  std::unique_ptr<SettingsView> _settings;
  base::Timer _trimTimer;
  rpl::lifetime _sessionLifetime;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
