#include "main_window.hpp"

#include <gdk/wayland/gdkwayland.h>

#include "chat_list.hpp"
#include "chat_view.hpp"
#include "calls.hpp"
#include "dialogs.hpp"
#include "login.hpp"
#include "scroll_helpers.hpp"
#include "settings.hpp"
#include "tray.hpp"

#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_folder.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

namespace Contrail {

MainWindow::MainWindow() {
  _window = adw_window_new();
  gtk_window_set_title(GTK_WINDOW(_window), "Contrail");
  gtk_window_set_default_size(GTK_WINDOW(_window), 900, 700);
  gtk_widget_set_size_request(_window, 320, 294);
  // Every press, motion and release passes here first: taps on links and photos are told apart
  // from scrolling (see TrackTap). A plain handler: this runs for every motion event.
  auto *taps = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(taps, GTK_PHASE_CAPTURE);
  g_signal_connect(taps, "event", G_CALLBACK(+[](GtkEventControllerLegacy *, GdkEvent *event, gpointer) -> gboolean {
    TrackTap(event);
    return FALSE;
  }), nullptr);
  gtk_widget_add_controller(_window, taps);

  _toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  _stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  auto *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  _callBanner = adw_banner_new("");
  adw_banner_set_use_markup(ADW_BANNER(_callBanner), FALSE);
  adw_banner_set_button_label(ADW_BANNER(_callBanner), U8(tr::lng_call_box_title(tr::now)).c_str());
  Connect<void(AdwBanner *)>(_callBanner, "button-clicked", [=](AdwBanner *) {
    if (_calls) _calls->present();
  });
  gtk_box_append(GTK_BOX(body), _callBanner);
  gtk_widget_set_vexpand(GTK_WIDGET(_stack), TRUE);
  gtk_box_append(GTK_BOX(body), GTK_WIDGET(_stack));
  adw_toast_overlay_set_child(_toasts, body);
  adw_window_set_content(ADW_WINDOW(_window), GTK_WIDGET(_toasts));

  // Starting up: the core reads its local storage.
  auto *loading = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(loading), adw_header_bar_new());
  auto *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 48, 48);
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(loading), spinner);
  gtk_stack_add_named(_stack, loading, "loading");

  // Chats: a sidebar with the chat list and a content pane with the open chat. Collapses into one
  // column on a phone.
  _split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
  adw_navigation_split_view_set_min_sidebar_width(_split, 300);
  adw_navigation_split_view_set_max_sidebar_width(_split, 400);
  _sidebarNav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  auto *sidebarPage = adw_navigation_page_new(GTK_WIDGET(_sidebarNav), "Contrail");
  adw_navigation_split_view_set_sidebar(_split, sidebarPage);

  auto *placeholderView = adw_toolbar_view_new();
  auto *placeholderHeader = adw_header_bar_new();
  adw_header_bar_set_show_title(ADW_HEADER_BAR(placeholderHeader), FALSE);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(placeholderView), placeholderHeader);
  auto *status = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "contrail-chats-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(status), U8(tr::lng_willbe_history(tr::now)).c_str());
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
  adw_window_add_breakpoint(ADW_WINDOW(_window), breakpoint);

  // Back from a chat on a phone: forget it, so its messages stop being marked as read.
  Connect<void(GObject *, GParamSpec *)>(_split, "notify::show-content", [this](GObject *, GParamSpec *) {
    if (adw_navigation_split_view_get_collapsed(_split) && !adw_navigation_split_view_get_show_content(_split)) {
      crl::on_main(this, [=] {
        if (adw_navigation_split_view_get_collapsed(_split)
            && !adw_navigation_split_view_get_show_content(_split)) {
          closeChat();
        }
      });
    }
  });

  // Closing the window quits, the core saves everything and exits the process; or, when the app runs
  // in the background, only hides the window: messages, notifications and calls keep coming.
  Connect<gboolean(GtkWindow *)>(_window, "close-request", [this](GtkWindow *) -> gboolean {
    if (Core::App().settings().closeBehavior() == Core::Settings::CloseBehavior::RunInBackground) {
      gtk_widget_set_visible(_window, FALSE);
    } else {
      Core::Quit();
    }
    return TRUE;
  });

  // The Wayland app ID matches the desktop entry, so the shell shows the right name and icon.
  Connect<void(GtkWidget *)>(_window, "realize", [](GtkWidget *window) {
    auto *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (GDK_IS_WAYLAND_TOPLEVEL(surface)) {
      gdk_wayland_toplevel_set_application_id(GDK_WAYLAND_TOPLEVEL(surface), kAppId);
    }
  });

  setupShortcuts();

  Core::App().domain().activeValue() | rpl::on_next([=](Main::Account *account) {
    showAccount(account);
  }, _lifetime);
  followCoreWindow();
  _calls = std::make_unique<CallView>(this);
  updateBackgroundMode();
  Core::App().unreadBadgeChanges() | rpl::on_next([=] {
    if (_tray) _tray->setUnread(Core::App().unreadBadgeMuted() ? 0 : Core::App().unreadBadge());
  }, _lifetime);
}

void MainWindow::updateBackgroundMode() {
  const auto background = Core::App().settings().closeBehavior() == Core::Settings::CloseBehavior::RunInBackground;
  if (!background) {
    _tray = nullptr;
    // Nothing would bring a hidden window back.
    if (!gtk_widget_get_visible(_window)) present();
    return;
  }
  if (_tray) return;
  // Not within the D-Bus call that asks for it: quitting destroys the icon.
  _tray = std::make_unique<TrayIcon>(TrayIcon::Callbacks{
      .toggle = [=] { crl::on_main(this, [=] {
        if (isActive()) gtk_widget_set_visible(_window, FALSE);
        else present();
      }); },
      .open = [=] { crl::on_main(this, [=] { present(); }); },
      .quit = [=] { crl::on_main(this, [] { Core::Quit(); }); },
  });
  _tray->setUnread(Core::App().unreadBadgeMuted() ? 0 : Core::App().unreadBadge());
}

// Telegram Desktop opens chats in its own (hidden) window: from a notification, a t.me link, a mention.
// Open the same chat here and bring this window to the front.
void MainWindow::followCoreWindow() {
  const auto window = Core::App().activePrimaryWindow();
  if (!window) {
    return;
  }
  window->sessionControllerValue() | rpl::map([](Window::SessionController *controller) {
    return controller ? controller->activeChatEntryChanges() : rpl::never<Dialogs::RowDescriptor>();
  }) | rpl::flatten_latest() | rpl::on_next([=](const Dialogs::RowDescriptor &row) {
    const auto thread = row.key.thread();
    const auto history = thread ? thread->owningHistory().get() : nullptr;
    if (!history || !_session || &history->session() != _session) {
      return;
    }
    if (const auto topic = row.key.topic()) {
      openThread(topic, row.fullId.msg);
    } else {
      openHistory(history, row.fullId.msg);
    }
    present();
    // Close it there, so it isn't read in the background and opens again next time.
    crl::on_main(this, [peer = history->peer] {
      Core::App().closeChatFromWindows(peer);
      HideCoreWindow();
    });
  }, _lifetime);
}

MainWindow::~MainWindow() {
  _lifetime.destroy();
  _sessionLifetime.destroy();
  _tray = nullptr;
  _calls = nullptr;
  clearSessionUi();
  g_object_unref(_placeholder);
}

bool MainWindow::isActive() const {
  return gtk_window_is_active(GTK_WINDOW(_window)) && gtk_widget_get_visible(_window);
}

void MainWindow::present() {
  gtk_window_present(GTK_WINDOW(_window));
}

void MainWindow::showToast(const QString &text) {
  auto *toast = adw_toast_new(Escape(text).c_str());
  adw_toast_set_timeout(toast, 3);
  adw_toast_overlay_add_toast(_toasts, toast);
}

void MainWindow::showAccount(Main::Account *account) {
  _sessionLifetime.destroy();
  clearSessionUi();
  if (!account) {
    gtk_stack_set_visible_child_name(_stack, "loading");
    return;
  }
  account->sessionValue() | rpl::on_next([=](Main::Session *session) {
    clearSessionUi();
    if (session) {
      showSession(session);
    } else {
      showLogin(account);
    }
  }, _sessionLifetime);
}

void MainWindow::clearSessionUi() {
  // Detach the widgets first: a view is destroyed only after its widgets are out of the window.
  adw_navigation_page_set_child(_contentPage, _placeholder);
  _chat = nullptr;
  adw_navigation_page_set_title(_contentPage, "Contrail");
  if (adw_navigation_view_find_page(_sidebarNav, "chats")) {
    adw_navigation_view_pop_to_tag(_sidebarNav, "chats");
  }
  _settings = nullptr;
  _archive = nullptr;
  _forum = nullptr;
  if (_chatList) {
    adw_navigation_view_remove(_sidebarNav, _chatList->page());
    _chatList = nullptr;
  }
  if (_login) {
    gtk_stack_remove(_stack, _login->widget());
    _login = nullptr;
  }
  _session = nullptr;
}

void MainWindow::showLogin(not_null<Main::Account *> account) {
  _login = std::make_unique<LoginView>(account);
  gtk_stack_add_named(_stack, _login->widget(), "login");
  gtk_stack_set_visible_child_name(_stack, "login");

  // First start: switch to the system's language, which Telegram suggests from the system locale.
  // Before signing in it applies without a restart; the sign-in pages are rebuilt in it.
  const auto marker = cWorkingDir() + u"tdata/contrail_language"_q;
  if (QFile::exists(marker)) {
    return;
  }
  const auto apply = [=] {
    if (QFile::exists(marker)) {
      return;
    }
    QDir().mkpath(cWorkingDir() + u"tdata"_q);
    QFile file(marker);
    if (!file.open(QIODevice::WriteOnly)) return;
    const auto suggested = Lang::CurrentCloudManager().suggestedLanguage();
    if (!suggested.isEmpty() && Lang::LanguageIdOrDefault(Lang::Id()) != suggested) {
      Lang::CurrentCloudManager().switchToLanguage(suggested);
    }
  };
  if (!Lang::CurrentCloudManager().suggestedLanguage().isEmpty()) {
    apply();
  } else {
    Lang::CurrentCloudManager().firstLanguageSuggestion() | rpl::take(1) | rpl::on_next(apply, _sessionLifetime);
  }
  Lang::Updated() | rpl::on_next([=] {
    if (_login && !_session) {
      gtk_stack_remove(_stack, _login->widget());
      _login = std::make_unique<LoginView>(account);
      gtk_stack_add_named(_stack, _login->widget(), "login");
      gtk_stack_set_visible_child_name(_stack, "login");
    }
  }, _sessionLifetime);
}

void MainWindow::showSession(not_null<Main::Session *> session) {
  _session = session;
  _chatList = std::make_unique<ChatList>(session, nullptr, this);
  adw_navigation_view_add(_sidebarNav, _chatList->page());
  gtk_stack_set_visible_child_name(_stack, "chats");
}

void MainWindow::openHistory(not_null<History *> history, MsgId showAt) {
  if (!_session) {
    return;
  }
  if (const auto forum = history->peer->forum()) {
    // Messages of a group with topics are written and read in their topics.
    const auto item = showAt ? history->owner().message(history->peer, showAt) : nullptr;
    if (const auto topic = item ? item->topic() : nullptr) {
      openThread(topic, showAt);
    } else {
      openForum(history);
    }
    return;
  }
  openThread(history, showAt);
}

void MainWindow::openThread(not_null<Data::Thread *> thread, MsgId showAt) {
  if (!_session) {
    return;
  }
  if (_chat && _chat->thread() == thread) {
    if (showAt) {
      _chat->showAt(showAt);
    }
  } else {
    auto old = std::move(_chat);
    _chat = std::make_unique<ChatView>(thread, this, showAt);
    adw_navigation_page_set_child(_contentPage, _chat->widget());
    const auto topic = thread->asTopic();
    adw_navigation_page_set_title(_contentPage, U8(topic ? topic->title() : thread->peer()->name()).c_str());
    old = nullptr;
  }
  if (const auto topic = thread->asTopic(); topic && (!_forum || _forum->forum() != topic->forum())) {
    // A topic opened from a notification or a link: Back on a phone leads to the other topics.
    openForum(topic->history());
  }
  if (_chatList) {
    _chatList->setCurrent(thread);
  }
  if (_archive) {
    _archive->setCurrent(thread);
  }
  if (_forum) {
    _forum->setCurrent(thread);
  }
  adw_navigation_split_view_set_show_content(_split, TRUE);
}

void MainWindow::openPeer(not_null<PeerData *> peer) {
  openHistory(peer->owner().history(peer));
}

void MainWindow::openForum(not_null<History *> history) {
  const auto forum = history->peer->forum();
  if (!_session || !forum) {
    return;
  }
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (_forum && _forum->forum() == forum) {
    if (adw_navigation_view_find_page(_sidebarNav, "forum")) {
      adw_navigation_view_pop_to_page(_sidebarNav, _forum->page());
    } else {
      adw_navigation_view_push(_sidebarNav, _forum->page());
    }
    return;
  }
  if (_forum) {
    // Another group's topics: this one takes their place.
    if (adw_navigation_view_find_page(_sidebarNav, "forum")) {
      adw_navigation_view_pop_to_page(_sidebarNav, _forum->page());
      adw_navigation_view_pop(_sidebarNav);
    }
    _forum = nullptr;
  }
  _forum = std::make_unique<ChatList>(_session, nullptr, this, forum);
  _forum->setCurrent(currentThread());
  adw_navigation_view_push(_sidebarNav, _forum->page());
  Connect<void(AdwNavigationPage *)>(_forum->page(), "hidden", [this, page = _forum->page()](AdwNavigationPage *) {
    crl::on_main(this, [=] {
      if (_forum && _forum->page() == page && adw_navigation_view_find_page(_sidebarNav, "forum") != page) {
        _forum = nullptr;
      }
    });
  });
}

void MainWindow::closeForum(Data::Forum *forum) {
  if (!_forum || _forum->forum() != forum) {
    return;
  }
  if (const auto topic = currentThread() ? currentThread()->asTopic() : nullptr; topic && topic->forum() == forum) {
    closeChat();
  }
  if (adw_navigation_view_find_page(_sidebarNav, "forum") == _forum->page()) {
    adw_navigation_view_pop_to_page(_sidebarNav, _forum->page());
    adw_navigation_view_pop(_sidebarNav);
  }
  _forum = nullptr;
}

History *MainWindow::currentHistory() const {
  return _chat ? _chat->history().get() : nullptr;
}

Data::Thread *MainWindow::currentThread() const {
  return _chat ? _chat->thread().get() : nullptr;
}

void MainWindow::closeChat() {
  if (!_chat) {
    return;
  }
  adw_navigation_page_set_child(_contentPage, _placeholder);
  adw_navigation_page_set_title(_contentPage, "Contrail");
  _chat = nullptr;
  if (_chatList) {
    _chatList->setCurrent(nullptr);
  }
  if (_archive) {
    _archive->setCurrent(nullptr);
  }
  if (_forum) {
    _forum->setCurrent(nullptr);
  }
}

void MainWindow::openSettings() {
  if (!_session) {
    return;
  }
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (_settings) {
    if (adw_navigation_view_find_page(_sidebarNav, "settings")) {
      adw_navigation_view_pop_to_page(_sidebarNav, _settings->page());
    } else {
      adw_navigation_view_push(_sidebarNav, _settings->page());
    }
    return;
  }
  _settings = std::make_unique<SettingsView>(_session, this);
  adw_navigation_view_push(_sidebarNav, _settings->page());
  Connect<void(AdwNavigationPage *)>(_settings->page(), "hidden", [this, page = _settings->page()](AdwNavigationPage *) {
    crl::on_main(this, [=] {
      // A sidebar page also becomes hidden when a chat covers it on a phone.
      // Release its owner only after it actually leaves the navigation stack.
      if (_settings && _settings->page() == page
          && adw_navigation_view_find_page(_sidebarNav, "settings") != page) {
        _settings = nullptr;
      }
    });
  });
}

void MainWindow::openArchive() {
  if (!_session) {
    return;
  }
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (_archive) {
    if (adw_navigation_view_find_page(_sidebarNav, "archive")) {
      adw_navigation_view_pop_to_page(_sidebarNav, _archive->page());
    } else {
      adw_navigation_view_push(_sidebarNav, _archive->page());
    }
    return;
  }
  _archive = std::make_unique<ChatList>(_session, _session->data().folder(Data::Folder::kId), this);
  adw_navigation_view_push(_sidebarNav, _archive->page());
  Connect<void(AdwNavigationPage *)>(_archive->page(), "hidden", [this, page = _archive->page()](AdwNavigationPage *) {
    crl::on_main(this, [=] {
      if (_archive && _archive->page() == page
          && adw_navigation_view_find_page(_sidebarNav, "archive") != page) {
        _archive = nullptr;
      }
    });
  });
}

void MainWindow::openContacts() {
  if (_session) {
    adw_navigation_split_view_set_show_content(_split, FALSE);
    adw_navigation_view_push(_sidebarNav, MakeContactsPage(_session, this));
  }
}

void MainWindow::openProfile(not_null<PeerData *> peer) {
  ShowProfile(this, peer);
}

void MainWindow::openPhoto(not_null<PhotoData *> photo, FullMsgId origin) {
  ShowPhoto(this, photo, origin);
}

void MainWindow::showAbout() {
  ShowAbout(this);
}

void MainWindow::confirmDeleteChat(not_null<PeerData *> peer) {
  ConfirmDeleteChat(this, peer);
}

void MainWindow::searchChats(const QString &query) {
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (adw_navigation_view_find_page(_sidebarNav, "chats")) {
    adw_navigation_view_pop_to_tag(_sidebarNav, "chats");
  }
  if (_chatList) {
    _chatList->startSearch(query);
  }
}

void MainWindow::sendBotCommand(const QString &command) {
  if (_chat) {
    _chat->sendText(command);
  }
}

void MainWindow::startCall(not_null<PeerData *> peer, bool video) {
  if (_session && &peer->session() == _session && _calls) {
    if (const auto user = peer->asUser()) _calls->start(user, video);
  }
}

void MainWindow::setCallStatus(const QString &text, bool active) {
  adw_banner_set_title(ADW_BANNER(_callBanner), U8(text).c_str());
  adw_banner_set_revealed(ADW_BANNER(_callBanner), active);
}

void MainWindow::debugCallPreview(bool incoming, bool video, const QString &name) {
  if (_calls) _calls->preview(incoming, video, name);
}

void MainWindow::debugLocalCall(const QString &action) {
  if (_calls) _calls->debugLocalCall(action);
}

void MainWindow::debugCheckDraft() {
  if (_chat) _chat->debugCheckDraft();
}

void MainWindow::debugLogin(const QString &step, const QString &value) {
  if (_login) _login->debugSubmit(step, value);
}

void MainWindow::debugHistory(const QString &action) {
  if (_chat) _chat->debugHistory(action);
}

void MainWindow::debugOpenChat(int index) {
  if (_chatList) {
    _chatList->openRow(index);
  }
}

void MainWindow::debugOpenTopic(int index) {
  if (_forum) {
    _forum->openRow(index);
  }
}

void MainWindow::debugBack() {
  if (adw_navigation_split_view_get_show_content(_split) && adw_navigation_split_view_get_collapsed(_split)) {
    adw_navigation_split_view_set_show_content(_split, FALSE);
  } else {
    adw_navigation_view_pop(_sidebarNav);
  }
}

void MainWindow::debugScroll(bool top) {
  if (_chat) {
    _chat->debugScroll(top);
  }
}

void MainWindow::setupShortcuts() {
  auto *controller = gtk_shortcut_controller_new();
  gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(controller), GTK_SHORTCUT_SCOPE_GLOBAL);
  auto add = [&](const char *trigger, void (*fn)(MainWindow *)) {
    auto *action = gtk_callback_action_new(
        +[](GtkWidget *, GVariant *, gpointer data) -> gboolean {
          auto *pair = static_cast<std::pair<MainWindow *, void (*)(MainWindow *)> *>(data);
          RunCoreEvent([&] { pair->second(pair->first); });
          return TRUE;
        },
        new std::pair<MainWindow *, void (*)(MainWindow *)>(this, fn),
        +[](gpointer data) { delete static_cast<std::pair<MainWindow *, void (*)(MainWindow *)> *>(data); });
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller),
                                         gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger), action));
  };
  add("<Control>q", [](MainWindow *) { Core::Quit(); });
  add("<Control>comma", [](MainWindow *w) { w->openSettings(); });
  add("Escape", [](MainWindow *w) {
    if (w->_chat && !w->_chat->handleEscape()) {
      if (adw_navigation_split_view_get_collapsed(w->_split)) {
        adw_navigation_split_view_set_show_content(w->_split, FALSE);
      } else {
        w->closeChat();
      }
    }
  });
  add("<Control>f", [](MainWindow *w) {
    if (w->_chatList) {
      w->_chatList->startSearch();
    }
  });
  gtk_widget_add_controller(_window, controller);
}

}  // namespace Contrail
