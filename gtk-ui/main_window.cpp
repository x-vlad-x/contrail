#include "main_window.hpp"

#include <gdk/wayland/gdkwayland.h>

#include "chat_list.hpp"
#include "chat_view.hpp"
#include "dialogs.hpp"
#include "login.hpp"
#include "settings.hpp"

#include "core/application.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
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
  gtk_widget_set_size_request(_window, 360, 294);

  _toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  _stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  adw_toast_overlay_set_child(_toasts, GTK_WIDGET(_stack));
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
  _contentPage = ADW_NAVIGATION_PAGE(adw_navigation_page_new(_placeholder, ""));
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
      g_idle_add_once(
          +[](gpointer self) {
            auto *w = static_cast<MainWindow *>(self);
            if (!adw_navigation_split_view_get_show_content(w->_split)) {
              w->closeChat();
            }
          },
          this);
    }
  });

  // Closing the window quits: the core saves everything and exits the process.
  Connect<gboolean(GtkWindow *)>(_window, "close-request", [](GtkWindow *) -> gboolean {
    Core::Quit();
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
    const auto history = row.key.history();
    if (!history || !_session || &history->session() != _session) {
      return;
    }
    openHistory(history, row.fullId.msg);
    present();
    // Close it there, so it isn't read in the background and opens again next time.
    crl::on_main(this, [peer = history->peer] {
      Core::App().closeChatFromWindows(peer);
      HideCoreWindow();
    });
  }, _lifetime);
}

MainWindow::~MainWindow() = default;

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
  adw_navigation_page_set_title(_contentPage, "");
  if (adw_navigation_view_find_page(_sidebarNav, "chats")) {
    adw_navigation_view_pop_to_tag(_sidebarNav, "chats");
  }
  _settings = nullptr;
  _archive = nullptr;
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
    file.open(QIODevice::WriteOnly);
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
  if (_chat && _chat->history() == history) {
    if (showAt) {
      _chat->showAt(showAt);
    }
  } else {
    auto old = std::move(_chat);
    _chat = std::make_unique<ChatView>(history, this, showAt);
    adw_navigation_page_set_child(_contentPage, _chat->widget());
    adw_navigation_page_set_title(_contentPage, U8(history->peer->name()).c_str());
    old = nullptr;
  }
  if (_chatList) {
    _chatList->setCurrent(history);
  }
  if (_archive) {
    _archive->setCurrent(history);
  }
  adw_navigation_split_view_set_show_content(_split, TRUE);
}

void MainWindow::openPeer(not_null<PeerData *> peer) {
  openHistory(peer->owner().history(peer));
}

History *MainWindow::currentHistory() const {
  return _chat ? _chat->history().get() : nullptr;
}

void MainWindow::closeChat() {
  if (!_chat) {
    return;
  }
  adw_navigation_page_set_child(_contentPage, _placeholder);
  adw_navigation_page_set_title(_contentPage, "");
  _chat = nullptr;
  if (_chatList) {
    _chatList->setCurrent(nullptr);
  }
  if (_archive) {
    _archive->setCurrent(nullptr);
  }
}

void MainWindow::openSettings() {
  if (!_session) {
    return;
  }
  _settings = std::make_unique<SettingsView>(_session, this);
  adw_navigation_view_push(_sidebarNav, _settings->page());
  Connect<void(AdwNavigationPage *)>(_settings->page(), "hidden", [this](AdwNavigationPage *) {
    g_idle_add_once(+[](gpointer self) { static_cast<MainWindow *>(self)->_settings = nullptr; }, this);
  });
}

void MainWindow::openArchive() {
  if (!_session) {
    return;
  }
  _archive = std::make_unique<ChatList>(_session, _session->data().folder(Data::Folder::kId), this);
  adw_navigation_view_push(_sidebarNav, _archive->page());
  Connect<void(AdwNavigationPage *)>(_archive->page(), "hidden", [this](AdwNavigationPage *) {
    g_idle_add_once(+[](gpointer self) { static_cast<MainWindow *>(self)->_archive = nullptr; }, this);
  });
}

void MainWindow::openContacts() {
  if (_session) {
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

void MainWindow::debugOpenChat(int index) {
  if (_chatList) {
    _chatList->openRow(index);
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
          pair->second(pair->first);
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
