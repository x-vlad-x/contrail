#include "main_window.hpp"

#include "scroll_helpers.hpp"

#include "chat_list.hpp"
#include "chat_view.hpp"
#include "local_state.hpp"
#include "login.hpp"
#include "memory_guard.hpp"
#include "memory_trim.hpp"
#include "network.hpp"
#include "notifications.hpp"
#include "proxy.hpp"
#include "settings.hpp"
#include "td/message_text.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"
#include "topic_list.hpp"
#include "tray.hpp"
#include "userpics.hpp"

namespace Contrail {
namespace {

namespace api = Td::api;

// The first start without a network gets no language pack: the built-in English comes up after this long.
constexpr auto kStringsTimeoutMs = 4000;
// Gives memory back once the start is over (see showChats).
constexpr auto kTrimAfterStartMs = 8000;

GtkWidget *MakeLoadingPage() {
  auto *page = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(page), adw_header_bar_new());
  auto *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 48, 48);
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(page), spinner);
  return page;
}

void AlertInWindow(GtkWidget *window, const std::string &heading, const std::string &body) {
  auto *dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", Tr("lng_box_ok").c_str());
  adw_dialog_present(ADW_DIALOG(dialog), window);
}

}  // namespace

MainWindow::MainWindow(GtkApplication *application)
    : _application(application),
      _memoryGuard(std::make_unique<MemoryGuard>([this](MemoryLevel level) { releaseMemory(level); })) {
  buildWindow();
  backgroundChanged();
}

void MainWindow::buildWindow() {
  _window = adw_application_window_new(_application);
  // GTK destroys it itself when it is closed to quit.
  g_object_add_weak_pointer(G_OBJECT(_window), reinterpret_cast<gpointer *>(&_window));
  // Every press, motion and release passes here before the widgets under the finger: see TrackTap.
  auto *taps = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(taps, GTK_PHASE_CAPTURE);
  g_signal_connect(taps, "event", G_CALLBACK(+[](GtkEventControllerLegacy *, GdkEvent *event, gpointer) -> gboolean {
    TrackTap(event);
    return FALSE;
  }), nullptr);
  gtk_widget_add_controller(_window, taps);
  gtk_window_set_title(GTK_WINDOW(_window), "Contrail TD");
  gtk_window_set_default_size(GTK_WINDOW(_window), 900, 700);
  gtk_widget_set_size_request(_window, 320, 294);

  _toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  _stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  adw_toast_overlay_set_child(_toasts, GTK_WIDGET(_stack));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(_window), GTK_WIDGET(_toasts));
  gtk_stack_add_named(_stack, MakeLoadingPage(), "loading");
  buildChats();

  // Telegram shows the account as online while the window is in front, and only then.
  // The window itself, not _window: this comes while a window going to the background is destroyed too.
  Connect<void(GObject *, GParamSpec *)>(_window, "notify::is-active", [this](GObject *window, GParamSpec *) {
    const auto active = gtk_window_is_active(GTK_WINDOW(window));
    if (_session) _session->setOnline(active);
    // Out of sight it may stay for days: let it hold no more than it uses.
    if (!active) TrimMemorySoon();
    _memoryGuard->setForeground(active);
  });
  // Closing the window quits, TDLib closes its database and the process exits; or, when the app runs in the
  // background, releases the interface: messages, notifications and calls keep coming.
  Connect<gboolean(GtkWindow *)>(_window, "close-request", [this](GtkWindow *) -> gboolean {
    if (!Settings().background) return FALSE;
    // Not while GTK is handling the window's own signal.
    g_idle_add_once(+[](gpointer data) { static_cast<MainWindow *>(data)->closeToBackground(); }, this);
    return TRUE;
  });
  _memoryGuard->setForeground(true);
}

// A sidebar with the chat list and a content pane with the open chat. Collapses into one column on a phone.
void MainWindow::buildChats() {
  _split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
  adw_navigation_split_view_set_min_sidebar_width(_split, 300);
  adw_navigation_split_view_set_max_sidebar_width(_split, 400);
  _sidebarNav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  // On quitting the window can go before the chats it shows: then they have nowhere to be removed from.
  g_object_add_weak_pointer(G_OBJECT(_sidebarNav), reinterpret_cast<gpointer *>(&_sidebarNav));
  adw_navigation_split_view_set_sidebar(_split, adw_navigation_page_new(GTK_WIDGET(_sidebarNav), "Contrail"));

  auto *placeholderView = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(placeholderView), header);
  auto *status = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "contrail-chats-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(status), Tr("lng_willbe_history").c_str());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(placeholderView), status);
  _placeholder = placeholderView;
  // The page releases its child when a chat replaces it. Keep the empty view alive for Back.
  g_object_ref_sink(_placeholder);
  _contentPage = ADW_NAVIGATION_PAGE(adw_navigation_page_new(_placeholder, "Contrail"));
  adw_navigation_split_view_set_content(_split, _contentPage);
  gtk_stack_add_named(_stack, GTK_WIDGET(_split), "chats");

  auto *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600sp"));
  GValue collapsed = G_VALUE_INIT;
  g_value_init(&collapsed, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed, TRUE);
  adw_breakpoint_add_setter(breakpoint, G_OBJECT(_split), "collapsed", &collapsed);
  g_value_unset(&collapsed);
  adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(_window), breakpoint);

  // Back from a chat on a phone: forget it, so its messages stop being marked as read.
  Connect<void(GObject *, GParamSpec *)>(_split, "notify::show-content", [this](GObject *, GParamSpec *) {
    if (!adw_navigation_split_view_get_collapsed(_split) || adw_navigation_split_view_get_show_content(_split)) return;
    g_idle_add_once(+[](gpointer data) {
      auto *self = static_cast<MainWindow *>(data);
      if (self->_split && adw_navigation_split_view_get_collapsed(self->_split)
          && !adw_navigation_split_view_get_show_content(self->_split)) {
        self->closeChat();
      }
    }, this);
  });
}

MainWindow::~MainWindow() {
  if (_stringsTimeout) g_source_remove(_stringsTimeout);
  _tray = nullptr;
  clearChats();
  if (_sidebarNav) g_object_remove_weak_pointer(G_OBJECT(_sidebarNav), reinterpret_cast<gpointer *>(&_sidebarNav));
  if (_window) g_object_remove_weak_pointer(G_OBJECT(_window), reinterpret_cast<gpointer *>(&_window));
  if (_placeholder) g_object_unref(_placeholder);
}

void MainWindow::clearChats() {
  clearViews();
  _forwardDrafts.clear();
  _notifier = nullptr;
  _network = nullptr;
  _unreadLifetime.reset();
  _unreadUnmuted = 0;
  updateTray();
  _authorized = false;
  _saved = std::nullopt;
  _store = nullptr;
}

// What shows the account goes; the store and what waits to be forwarded stay.
void MainWindow::clearViews() {
  dropForwardPicker();
  // Quitting, GTK may have destroyed the window and its pages already.
  if (_chat && _window) adw_navigation_page_set_child(_contentPage, _placeholder);
  _chat = nullptr;
  // The stack emptied at once, without the animation of going back: a page in the stack (the chat list at its
  // bottom too) is only removed once it has been popped.
  if (_sidebarNav && _chatList) {
    adw_navigation_view_replace(_sidebarNav, nullptr, 0);
    adw_navigation_view_remove(_sidebarNav, _chatList->page());
  }
  _proxy = nullptr;
  _settings = nullptr;
  _topics = nullptr;
  _archive = nullptr;
  _chatList = nullptr;
}

void MainWindow::present() {
  if (!_window) {
    buildWindow();
    // Otherwise the window waits for the next state: a session that ended in the background starts again.
    if (_authorized) {
      showChats();
      restoreChat();
    }
  }
  gtk_window_present(GTK_WINDOW(_window));
}

void MainWindow::showToast(const std::string &text) {
  if (g_getenv("CONTRAIL_DEBUG")) g_message("toast: %s", text.c_str());
  if (_toasts) adw_toast_overlay_add_toast(_toasts, adw_toast_new(text.c_str()));
}

void MainWindow::searchChats(const std::string &query) {
  if (!_chatList || !_sidebarNav) return;
  adw_navigation_split_view_set_show_content(_split, FALSE);
  // As Back would: the archive or a group's topics go first.
  adw_navigation_view_pop_to_page(_sidebarNav, _chatList->page());
  _chatList->startSearch(query);
}

void MainWindow::detach() {
  clearChats();
  _sessionLifetime.reset();
  _session = nullptr;
}

void MainWindow::attach(Td::Session &session, const std::string &language) {
  clearChats();
  _session = &session;
  _sessionLifetime.reset();
  // From the first update on: TDLib sends what it knows about chats only once.
  _store = std::make_unique<Td::Store>(session.client(), [this](const api::message &message) {
    return Td::MessagePreview(message, *_store);
  });
  _network = std::make_unique<NetworkWatch>(session.client());
  // The tray icon's badge: chats with unread messages that aren't muted, as Telegram Desktop counts them.
  session.client().subscribe([this](api::Update &update) {
    if (update.get_id() != api::updateUnreadChatCount::ID) return;
    auto &data = static_cast<api::updateUnreadChatCount &>(update);
    if (!data.chat_list_ || data.chat_list_->get_id() != api::chatListMain::ID) return;
    _unreadUnmuted = data.unread_unmuted_count_;
    updateTray();
  }, _unreadLifetime);
  _language = language;
  _stringsAsked = false;
  _stringsReady = false;
  _loginShown = false;
  _login = nullptr;
  gtk_stack_set_visible_child_name(_stack, "loading");
  session.setAuthorizationHandler([this](api::AuthorizationState &state) { authorization(state); });
}

void MainWindow::stringsReady() {
  if (_stringsTimeout) {
    g_source_remove(_stringsTimeout);
    _stringsTimeout = 0;
  }
  if (_stringsReady) return;
  _stringsReady = true;
  // The state that came in the meantime waited for this.
  _session->client().send(api::make_object<api::getAuthorizationState>(),
                          [this](Td::ObjectPtr object) {
                            if (object && object->get_id() != api::error::ID) {
                              authorization(static_cast<api::AuthorizationState &>(*object));
                            }
                          },
                          _sessionLifetime);
}

void MainWindow::authorization(api::AuthorizationState &state) {
  // The first state after the parameters: TDLib knows the language pack's options now.
  if (!_stringsAsked) {
    _stringsAsked = true;
    LoadStrings(_session->client(), _sessionLifetime, _language, [this] { stringsReady(); });
    if (!_stringsReady) {
      _stringsTimeout = g_timeout_add_once(kStringsTimeoutMs, +[](gpointer data) {
        auto *window = static_cast<MainWindow *>(data);
        window->_stringsTimeout = 0;
        window->stringsReady();
      }, this);
    }
  }
  if (!_stringsReady) return;
  _authorized = state.get_id() == api::authorizationStateReady::ID;
  if (_authorized && !_notifier) _notifier = std::make_unique<Notifier>(*_store, *this);
  if (!_authorized) _notifier = nullptr;
  if (!_window) {
    // Without the window only signing in again needs it back: Telegram ended the session in the background.
    if (_authorized || state.get_id() == api::authorizationStateLoggingOut::ID
        || state.get_id() == api::authorizationStateClosing::ID || state.get_id() == api::authorizationStateClosed::ID) {
      return;
    }
    buildWindow();
    gtk_window_present(GTK_WINDOW(_window));
  }
  switch (state.get_id()) {
    case api::authorizationStateWaitPhoneNumber::ID:
    case api::authorizationStateWaitCode::ID:
    case api::authorizationStateWaitEmailAddress::ID:
    case api::authorizationStateWaitEmailCode::ID:
    case api::authorizationStateWaitPassword::ID:
    case api::authorizationStateWaitRegistration::ID:
    case api::authorizationStateWaitPremiumPurchase::ID:
      showLogin(state);
      break;
    case api::authorizationStateReady::ID:
      showChats();
      break;
    case api::authorizationStateLoggingOut::ID:
    case api::authorizationStateClosing::ID:
    case api::authorizationStateClosed::ID:
      gtk_stack_set_visible_child_name(_stack, "loading");
      break;
    default: break;
  }
}

void MainWindow::showLogin(api::AuthorizationState &state) {
  if (!_login) {
    _login = std::make_unique<LoginView>(*_session);
    if (auto *old = gtk_stack_get_child_by_name(_stack, "login")) gtk_stack_remove(_stack, old);
    gtk_stack_add_named(_stack, _login->widget(), "login");
  }
  _loginShown = true;
  _login->show(state);
  gtk_stack_set_visible_child_name(_stack, "login");
}

void MainWindow::showChats() {
  if (!_chatList) {
    _chatList = std::make_unique<ChatList>(*_store, Td::ListKind::Main, *this);
    adw_navigation_view_add(_sidebarNav, _chatList->page());
    _store->loadMore(Td::ListKind::Main);
    // Starting, TDLib reads its database and frees most of what it read: 14 MiB on the phone. The first page of
    // chats is there within seconds; this waits for the rest of the start to be over.
    TrimMemorySoon(kTrimAfterStartMs);
  }
  gtk_stack_set_visible_child_name(_stack, "chats");
  if (_login) {
    gtk_stack_remove(_stack, _login->widget());
    _login = nullptr;
  }
  if (_loginShown) {
    _loginShown = false;
    warnAboutConfirmation();
  }
}

void MainWindow::setCurrent(Td::ChatId chat, std::int32_t topic) {
  // The chat list marks the group of an open topic.
  if (_chatList) _chatList->setCurrent(chat);
  if (_archive) _archive->setCurrent(chat);
  if (_topics) _topics->setCurrent(_topics->chat() == chat ? topic : 0);
}

Td::ChatId MainWindow::currentChat() const {
  return _chat ? _chat->chat() : 0;
}

std::int32_t MainWindow::currentTopic() const {
  return _chat ? _chat->topic() : 0;
}

void MainWindow::openChat(Td::ChatId chat, std::int64_t showAt) {
  if (!_store || !_window) return;
  const auto known = _store->chat(chat);
  if (!known) return;
  if (known->forum) {
    // Messages of a group with topics are written and read in their topics.
    if (!showAt) {
      openForum(chat);
      return;
    }
    _store->client().request<api::message>(
        api::make_object<api::getMessage>(chat, showAt), _sessionLifetime,
        [this, chat, showAt](api::message &message) {
          if (const auto topic = Td::MessageTopic(message)) openTopic(chat, topic, showAt);
          else openForum(chat);
        },
        [this, chat](api::error &) { openForum(chat); });
    return;
  }
  if (_chat && _chat->chat() == chat && !_chat->topic()) {
    if (showAt) _chat->showAt(showAt);
  } else {
    auto old = std::move(_chat);
    _chat = std::make_unique<ChatView>(*_store, chat, *this, showAt);
    adw_navigation_page_set_child(_contentPage, _chat->widget());
    adw_navigation_page_set_title(_contentPage, known->title.c_str());
    if (old) {
      old = nullptr;
      TrimMemorySoon();
    }
  }
  setCurrent(chat, 0);
  adw_navigation_split_view_set_show_content(_split, TRUE);
}

void MainWindow::openTopic(Td::ChatId chat, std::int32_t topic, std::int64_t showAt) {
  if (!_store || !_window || !_store->chat(chat) || !topic) return;
  // A topic opened from a link or a message elsewhere: Back on a phone leads to the other topics.
  if (!_topics || _topics->chat() != chat) openForum(chat);
  if (_chat && _chat->chat() == chat && _chat->topic() == topic) {
    if (showAt) _chat->showAt(showAt);
  } else {
    auto old = std::move(_chat);
    _chat = std::make_unique<ChatView>(*_store, chat, *this, showAt, topic);
    adw_navigation_page_set_child(_contentPage, _chat->widget());
    const auto known = _store->topic(chat, topic);
    adw_navigation_page_set_title(_contentPage, known ? known->row.title.c_str() : _store->chat(chat)->title.c_str());
    if (old) {
      old = nullptr;
      TrimMemorySoon();
    }
  }
  setCurrent(chat, topic);
  adw_navigation_split_view_set_show_content(_split, TRUE);
}

void MainWindow::openForum(Td::ChatId chat) {
  if (!_store || !_chatList || !_window) return;
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (_topics && _topics->chat() == chat) {
    if (adw_navigation_view_find_page(_sidebarNav, "forum")) {
      adw_navigation_view_pop_to_page(_sidebarNav, _topics->page());
    } else {
      adw_navigation_view_push(_sidebarNav, _topics->page());
    }
    return;
  }
  if (_topics) {
    // Another group's topics: these take their place.
    if (adw_navigation_view_find_page(_sidebarNav, "forum")) {
      adw_navigation_view_pop_to_page(_sidebarNav, _topics->page());
      adw_navigation_view_pop(_sidebarNav);
    }
    _topics = nullptr;
  }
  _topics = std::make_unique<TopicList>(*_store, chat, *this);
  _topics->setCurrent(currentChat() == chat ? currentTopic() : 0);
  adw_navigation_view_push(_sidebarNav, _topics->page());
  // Back from the topics: they go, unless they come back at once (a topic of the same group opened).
  Connect<void(AdwNavigationPage *)>(_topics->page(), "hidden", [this](AdwNavigationPage *) {
    g_idle_add_once(+[](gpointer data) {
      auto *self = static_cast<MainWindow *>(data);
      if (self->_topics && self->_sidebarNav
          && adw_navigation_view_find_page(self->_sidebarNav, "forum") != self->_topics->page()) {
        self->_topics = nullptr;
      }
    }, this);
  });
}

void MainWindow::closeForum(Td::ChatId chat) {
  if (!_window) return;
  if (currentChat() == chat && currentTopic()) {
    closeChat();
    adw_navigation_split_view_set_show_content(_split, FALSE);
  }
  if (!_topics || _topics->chat() != chat) return;
  if (_sidebarNav && adw_navigation_view_find_page(_sidebarNav, "forum") == _topics->page()) {
    adw_navigation_view_pop_to_page(_sidebarNav, _topics->page());
    adw_navigation_view_pop(_sidebarNav);
  }
  _topics = nullptr;
}

void MainWindow::closeChat() {
  if (!_chat) return;
  adw_navigation_page_set_child(_contentPage, _placeholder);
  adw_navigation_page_set_title(_contentPage, "Contrail");
  _chat = nullptr;
  setCurrent(0, 0);
  TrimMemorySoon();
}

void MainWindow::openArchive() {
  if (!_store || !_window) return;
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (!_archive) {
    _archive = std::make_unique<ChatList>(*_store, Td::ListKind::Archive, *this);
    _store->loadMore(Td::ListKind::Archive);
  }
  adw_navigation_view_push(_sidebarNav, _archive->page());
}

void MainWindow::warnAboutConfirmation() {
  AlertInWindow(_window,
                Translated("Confirm this login", "Подтвердите вход", "Підтвердьте вхід"),
                Translated("Telegram may ask “Is it you?” in the app on your other device. Answer right away: "
                           "an unconfirmed login is closed within minutes.",
                           "На другом вашем устройстве Telegram может спросить «Это вы?». Ответьте сразу: "
                           "неподтверждённый вход закрывается уже через несколько минут.",
                           "На іншому вашому пристрої Telegram може запитати «Це ви?». Дайте відповідь одразу: "
                           "непідтверджений вхід закривається вже за кілька хвилин."));
}

void MainWindow::showSessionLost() {
  present();
  AlertInWindow(_window,
                Translated("You were logged out", "Вы вышли из аккаунта", "Ви вийшли з акаунта"),
                Translated("Telegram ended this session and its data on this device was deleted. This happens to a "
                           "new login that was not confirmed on another device, or when the session is terminated "
                           "from another device.",
                           "Telegram завершил этот сеанс, его данные на устройстве удалены. Так бывает с новым "
                           "входом, который не подтвердили на другом устройстве, или когда сеанс завершён с "
                           "другого устройства.",
                           "Telegram завершив цей сеанс, його дані на пристрої видалено. Так буває з новим входом, "
                           "який не підтвердили на іншому пристрої, або коли сеанс завершено з іншого пристрою."));
}

void MainWindow::showForwardPicker(ForwardDraft draft) {
  if (!_store || !_window || draft.ids.empty()) return;
  dropForwardPicker();
  _forwardPicker = std::make_unique<ForwardPicker>(*_store, *this, std::move(draft));
  _forwardPicker->present();
}

// Not from inside the dialog's own signal.
void MainWindow::forwardPickerClosed() {
  if (_forwardPickerSource) return;
  _forwardPickerSource = g_idle_add_once(+[](gpointer data) {
    auto *self = static_cast<MainWindow *>(data);
    self->_forwardPickerSource = 0;
    self->_forwardPicker = nullptr;
  }, this);
}

void MainWindow::dropForwardPicker() {
  if (_forwardPickerSource) g_source_remove(std::exchange(_forwardPickerSource, 0));
  _forwardPicker = nullptr;
}

const ForwardDraft *MainWindow::forwardDraft(Td::ChatId chat, std::int32_t topic) const {
  const auto it = _forwardDrafts.find({chat, topic});
  return it == _forwardDrafts.end() ? nullptr : &it->second;
}

void MainWindow::setForwardDraft(Td::ChatId chat, std::int32_t topic, ForwardDraft draft) {
  if (draft.ids.empty()) {
    _forwardDrafts.erase({chat, topic});
  } else {
    _forwardDrafts[{chat, topic}] = std::move(draft);
  }
  if (_chat && _chat->chat() == chat && _chat->topic() == topic) _chat->forwardChanged();
}

void MainWindow::debugOpenChat(int index) {
  if (_chatList) _chatList->openRow(index);
}

void MainWindow::debugOpenTopic(int index) {
  if (_topics) _topics->openRow(index);
}

void MainWindow::debugBack() {
  if (!_window) return;
  if (adw_navigation_split_view_get_show_content(_split) && adw_navigation_split_view_get_collapsed(_split)) {
    adw_navigation_split_view_set_show_content(_split, FALSE);
  } else {
    adw_navigation_view_pop(_sidebarNav);
  }
}

void MainWindow::debugLogin(const std::string &step, const std::string &value) {
  if (_login) _login->debugSubmit(step, value);
}

// --- Settings and the app in the background -------------------------------------------------------------

void MainWindow::openSettings() {
  if (!_store || !_chatList || !_window) return;
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (_settings) {
    if (adw_navigation_view_find_page(_sidebarNav, "settings")) {
      adw_navigation_view_pop_to_page(_sidebarNav, _settings->page());
    } else {
      adw_navigation_view_push(_sidebarNav, _settings->page());
    }
    return;
  }
  _settings = std::make_unique<SettingsView>(*_store, *this);
  adw_navigation_view_push(_sidebarNav, _settings->page());
  // Back from the settings: they go. A sidebar page is also hidden when a chat covers it on a phone, so only once
  // it has left the navigation stack.
  Connect<void(AdwNavigationPage *)>(_settings->page(), "hidden", [this](AdwNavigationPage *) {
    g_idle_add_once(+[](gpointer data) {
      auto *self = static_cast<MainWindow *>(data);
      if (self->_settings && self->_sidebarNav
          && adw_navigation_view_find_page(self->_sidebarNav, "settings") != self->_settings->page()) {
        self->_settings = nullptr;
      }
    }, this);
  });
}

void MainWindow::openProxy() {
  if (!_store || !_settings || !_window) return;
  if (!_proxy) {
    _proxy = std::make_unique<ProxyView>(*_store, *this);
    Connect<void(AdwNavigationPage *)>(_proxy->page(), "hidden", [this](AdwNavigationPage *) {
      g_idle_add_once(+[](gpointer data) {
        auto *self = static_cast<MainWindow *>(data);
        if (self->_proxy && self->_sidebarNav
            && adw_navigation_view_find_page(self->_sidebarNav, "proxy") != self->_proxy->page()) {
          self->_proxy = nullptr;
        }
      }, this);
    });
  }
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (adw_navigation_view_find_page(_sidebarNav, "proxy")) {
    adw_navigation_view_pop_to_page(_sidebarNav, _proxy->page());
  } else {
    adw_navigation_view_push(_sidebarNav, _proxy->page());
  }
}

void MainWindow::showAbout() {
  if (_window) ShowAbout(_window);
}

bool MainWindow::isActive() const {
  return _window && gtk_widget_get_visible(_window) && gtk_window_is_active(GTK_WINDOW(_window));
}

void MainWindow::backgroundChanged() {
  const auto background = Settings().background;
  if (!background) {
    _tray = nullptr;
    // Nothing would bring a hidden window back; and without one the app would quit as soon as it lets go.
    if (!_window || !gtk_widget_get_visible(_window)) present();
    if (_held) {
      _held = false;
      g_application_release(G_APPLICATION(_application));
    }
    return;
  }
  if (!_held) {
    _held = true;
    g_application_hold(G_APPLICATION(_application));
  }
  if (_tray) return;
  // Not within the D-Bus call that asks for it: quitting destroys the icon.
  _tray = std::make_unique<TrayIcon>(TrayIcon::Callbacks{
      .toggle = [this] {
        g_idle_add_once(+[](gpointer data) {
          auto *self = static_cast<MainWindow *>(data);
          if (self->isActive()) self->closeToBackground();
          else self->present();
        }, this);
      },
      .open = [this] {
        g_idle_add_once(+[](gpointer data) { static_cast<MainWindow *>(data)->present(); }, this);
      },
      .quit = [this] {
        g_idle_add_once(+[](gpointer data) {
          g_application_quit(G_APPLICATION(static_cast<MainWindow *>(data)->_application));
        }, this);
      },
  });
  updateTray();
}

void MainWindow::updateTray() {
  if (_tray) _tray->setUnread(_unreadUnmuted);
}

void MainWindow::closeToBackground() {
  if (!_window) return;
  if (!_authorized) {
    gtk_widget_set_visible(_window, FALSE);
    return;
  }
  releaseWindow();
}

void MainWindow::releaseWindow() {
  _saved = SavedChat{currentChat(), currentTopic()};
  if (_chat) {
    const auto place = _chat->place();
    _saved->message = place.message;
    _saved->y = place.y;
  }
  clearViews();
  if (_sidebarNav) g_object_remove_weak_pointer(G_OBJECT(_sidebarNav), reinterpret_cast<gpointer *>(&_sidebarNav));
  _sidebarNav = nullptr;
  auto *window = std::exchange(_window, nullptr);
  g_object_remove_weak_pointer(G_OBJECT(window), reinterpret_cast<gpointer *>(&_window));
  gtk_window_destroy(GTK_WINDOW(window));
  _toasts = nullptr;
  _stack = nullptr;
  _split = nullptr;
  _contentPage = nullptr;
  g_object_unref(std::exchange(_placeholder, nullptr));
  if (_session) _session->setOnline(false);
  _memoryGuard->setForeground(false);
  // The pictures only the interface showed; the window's own memory goes once GTK has finalized it.
  ReleaseAvatars();
  TrimMemorySoon(3000);
  g_message("background: the window is released");
}

// Opening the app again comes back to the chat it had, as after closing the window in 0.3.1.
void MainWindow::restoreChat() {
  const auto saved = std::exchange(_saved, std::nullopt);
  if (!saved || !saved->chat || !_store || !_store->chat(saved->chat)) return;
  if (saved->topic) openTopic(saved->chat, saved->topic);
  else openChat(saved->chat);
  if (_chat && _chat->chat() == saved->chat) _chat->restorePlace({saved->message, saved->y});
}

void MainWindow::releaseMemory(MemoryLevel level) {
  if (level == MemoryLevel::Critical && _window && Settings().background && !isActive()
      && !adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(_window))) {
    closeToBackground();
  } else {
    ReleaseAvatars();
  }
  TrimMemorySoon(0);
}

}  // namespace Contrail
