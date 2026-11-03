#pragma once

#include "app.hpp"
#include "forward.hpp"
#include "memory_pressure.hpp"
#include "td/client.hpp"
#include "td/store.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Contrail {

class ChatList;
class ChatView;
class LoginView;
class MemoryGuard;
class NetworkWatch;
class Notifier;
class ProxyView;
class SettingsView;
class TrayIcon;
class TopicList;

// The application's window: a spinner while TDLib starts, the login, then the chats.
class MainWindow final {
 public:
  explicit MainWindow(GtkApplication *application);
  ~MainWindow();

  [[nodiscard]] GtkWidget *widget() const { return _window; }  // Null while the app runs without its window.
  void present();
  void showToast(const std::string &text);

  // Follows a new TDLib client: a lost session starts one with an empty database.
  void attach(Td::Session &session, const std::string &language);
  // Lets go of the client before it is destroyed.
  void detach();
  // Telegram ended the session: says why instead of just showing the login again.
  void showSessionLost();

  // On a phone the chat goes over the list. `showAt`: a message to show instead of the last place. A group with
  // topics opens its list of topics instead, or the topic of `showAt`.
  void openChat(Td::ChatId chat, std::int64_t showAt = 0);
  // A topic is a chat of its own; its group's topics go in the sidebar, so that Back on a phone leads to them.
  void openTopic(Td::ChatId chat, std::int32_t topic, std::int64_t showAt = 0);
  // The list of topics over the chat list.
  void openForum(Td::ChatId chat);
  // The group stopped being a forum: its topics and an open topic of it go.
  void closeForum(Td::ChatId chat);
  void closeChat();
  void openArchive();
  // A hashtag tapped in a message: the chat list searches for it (on a phone it comes over the chat).
  void searchChats(const std::string &query);
  [[nodiscard]] Td::ChatId currentChat() const;
  [[nodiscard]] std::int32_t currentTopic() const;  // 0 unless a topic is open.
  [[nodiscard]] TopicList *topicList() const { return _topics.get(); }
  [[nodiscard]] Td::Store *store() const { return _store.get(); }
  [[nodiscard]] ChatView *chatView() const { return _chat.get(); }
  [[nodiscard]] ChatList *chatList(bool archive = false) const { return archive ? _archive.get() : _chatList.get(); }

  // Messages to forward: the chats to choose from, then they wait in the chosen chat (or topic) until sent there.
  void showForwardPicker(ForwardDraft draft);
  void forwardPickerClosed();
  [[nodiscard]] ForwardPicker *forwardPicker() const { return _forwardPicker.get(); }
  // Null when nothing waits there. An empty draft takes away what waited.
  [[nodiscard]] const ForwardDraft *forwardDraft(Td::ChatId chat, std::int32_t topic) const;
  void setForwardDraft(Td::ChatId chat, std::int32_t topic, ForwardDraft draft);

  // The chat's card, the contacts in the sidebar, "About" (dialogs.cpp).
  void openProfile(Td::ChatId chat);
  void openContacts();
  void showAbout();
  // A page of the sidebar by its tag, if it is there.
  [[nodiscard]] AdwNavigationPage *sidebarPage(const char *tag) const;
  // Settings and the app in the background (settings.cpp, tray.cpp, memory_guard.cpp).
  void openSettings();
  [[nodiscard]] SettingsView *settingsView() const { return _settings.get(); }
  // The proxies, over the settings.
  void openProxy();
  [[nodiscard]] ProxyView *proxyView() const { return _proxy.get(); }
  // "Run in the background" was turned on or off: the tray icon, and whether closing the window quits.
  void backgroundChanged();
  // The window goes and its interface with it; TDLib, notifications and calls stay. Opening the app builds it
  // again on the same chat. While signing in the window is only hidden: what was typed there would be lost.
  void closeToBackground();
  [[nodiscard]] bool isActive() const;
  [[nodiscard]] TrayIcon *tray() const { return _tray.get(); }
  [[nodiscard]] Notifier *notifier() const { return _notifier.get(); }
  [[nodiscard]] NetworkWatch *networkWatch() const { return _network.get(); }
  [[nodiscard]] MemoryGuard &memoryGuard() const { return *_memoryGuard; }
  void releaseMemory(MemoryLevel level);

  void debugLogin(const std::string &step, const std::string &value);
  void debugOpenChat(int index);
  void debugOpenTopic(int index);
  void debugBack();

 private:
  void authorization(Td::api::AuthorizationState &state);
  void stringsReady();
  void showLogin(Td::api::AuthorizationState &state);
  void showChats();
  void buildChats();
  void clearChats();
  void setCurrent(Td::ChatId chat, std::int32_t topic);
  // A new login dies within minutes if nobody confirms it on another device.
  void warnAboutConfirmation();
  void dropForwardPicker();
  void buildWindow();
  void clearViews();
  void releaseWindow();
  void restoreChat();
  void updateTray();

  GtkWidget *_window = nullptr;
  AdwToastOverlay *_toasts = nullptr;
  GtkStack *_stack = nullptr;
  AdwNavigationSplitView *_split = nullptr;
  AdwNavigationView *_sidebarNav = nullptr;
  AdwNavigationPage *_contentPage = nullptr;
  GtkWidget *_placeholder = nullptr;
  Td::Session *_session = nullptr;
  std::unique_ptr<Td::Store> _store;
  std::unique_ptr<ChatList> _chatList;
  std::unique_ptr<ChatList> _archive;
  std::unique_ptr<TopicList> _topics;
  std::unique_ptr<ChatView> _chat;
  std::unique_ptr<ForwardPicker> _forwardPicker;
  guint _forwardPickerSource = 0;
  std::map<std::pair<Td::ChatId, std::int32_t>, ForwardDraft> _forwardDrafts;
  Td::Lifetime _sessionLifetime;
  std::string _language;
  guint _stringsTimeout = 0;
  bool _stringsAsked = false;
  bool _stringsReady = false;  // Pages are built with their text: nothing is shown before the strings.
  std::unique_ptr<LoginView> _login;
  bool _loginShown = false;  // This client went through the login: it isn't confirmed yet.

  // The app in the background.
  struct SavedChat {
    Td::ChatId chat = 0;
    std::int32_t topic = 0;
    std::int64_t message = 0;  // The place on the screen (ChatView::Place).
    double y = 0.;
  };
  GtkApplication *_application = nullptr;
  std::unique_ptr<SettingsView> _settings;
  std::unique_ptr<ProxyView> _proxy;
  std::unique_ptr<TrayIcon> _tray;
  std::unique_ptr<Notifier> _notifier;  // While signed in, with or without the window.
  std::unique_ptr<NetworkWatch> _network;  // With every client.
  std::unique_ptr<MemoryGuard> _memoryGuard;
  std::optional<SavedChat> _saved;  // The chat the window had when it went.
  Td::Lifetime _unreadLifetime;
  int _unreadUnmuted = 0;  // Chats with unread messages that notify: the tray icon's badge.
  bool _authorized = false;  // The chats can be shown: the window built again shows them at once.
  bool _held = false;  // The app holds itself without a window (g_application_hold).
};

}  // namespace Contrail
