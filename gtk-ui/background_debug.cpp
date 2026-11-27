#include "background_debug.hpp"

#include "local_state.hpp"
#include "chat_view.hpp"
#include "main_window.hpp"
#include "memory_guard.hpp"
#include "network.hpp"
#include "notifications.hpp"
#include "proxy.hpp"
#include "text.hpp"
#include "settings.hpp"
#include "tray.hpp"

#include <cstdlib>

namespace Contrail {

bool RunBackgroundCommand(MainWindow &window, const std::string &name, const std::string &rest) {
  if (name == "bg-state") {
    const auto *tray = window.tray();
    const auto place = window.chatView() ? window.chatView()->place() : ChatView::Place();
    const auto *blocker = window.restartBlocker();
    g_message("bg: window=%d visible=%d background=%d tray=%s unread=%d place=%lld/%.0f chat=%lld gpu=%d restart=%s",
              int(window.widget() != nullptr), window.widget() ? int(gtk_widget_get_visible(window.widget())) : 0,
              int(Settings().background), tray ? (tray->shown() ? "registered" : "waiting") : "none",
              tray ? tray->unread() : -1, static_cast<long long>(place.message), place.y,
              static_cast<long long>(window.currentChat()), int(SurfacesUsed()), blocker ? blocker : "ready");
  } else if (name == "bg-restart") {
    // The restart without the window, without waiting for its minute.
    window.debugRestart();
  } else if (name == "bg-close") {
    // As the close button does.
    if (window.widget()) gtk_window_close(GTK_WINDOW(window.widget()));
  } else if (name == "bg-open") {
    // As the launcher does.
    g_application_activate(g_application_get_default());
  } else if (name == "settings") {
    window.openSettings();
  } else if (name == "settings-state") {
    if (const auto *settings = window.settingsView()) settings->debugState();
    else g_message("settings: closed");
  } else if (name == "settings-set") {
    const auto gap = rest.find(' ');
    if (auto *settings = window.settingsView(); settings && gap != std::string::npos) {
      settings->debugSet(rest.substr(0, gap), std::atoi(rest.c_str() + gap + 1) != 0);
    }
  } else if (name == "settings-about") {
    window.showAbout();
  } else if (name == "settings-logout") {
    if (window.widget()) ConfirmLogOut(window.widget());
  } else if (name == "mem-guard") {
    g_message("mem-guard: %s", window.memoryGuard().debugState().c_str());
  } else if (name == "mem-simulate") {
    window.memoryGuard().debugSimulate(rest == "critical" ? MemoryLevel::Critical : MemoryLevel::Low);
  } else if (name == "net-state" || name == "net-simulate") {
    if (auto *network = window.networkWatch(); network && name == "net-state") network->debugState();
    else if (network) network->debugSimulate(rest);
  } else if (name == "notify-state") {
    if (const auto *notifier = window.notifier()) notifier->debugState();
    else g_message("notify: none");
  } else if (name == "notify-do") {
    // notify-do CHAT open|read|reply TEXT: as the notification's buttons do.
    const auto gap = rest.find(' ');
    const auto action = gap == std::string::npos ? std::string("open") : rest.substr(gap + 1);
    const auto space = action.find(' ');
    if (auto *notifier = window.notifier()) {
      notifier->debugAction(std::atoll(rest.c_str()), action.substr(0, space),
                            space == std::string::npos ? std::string() : action.substr(space + 1));
    }
  } else if (name == "proxy-link" || name == "link") {
    // As a tap on the link in a message.
    OpenTelegramLink(rest);
  } else if (name == "proxy") {
    window.openProxy();
  } else if (name.starts_with("proxy-")) {
    auto *proxy = window.proxyView();
    if (!proxy) {
      g_message("proxy: closed");
    } else if (name == "proxy-state") {
      proxy->debugState();
    } else if (name == "proxy-add") {
      proxy->debugAdd(rest);
    } else if (name == "proxy-edit") {
      const auto gap = rest.find(' ');
      proxy->debugAdd(gap == std::string::npos ? std::string() : rest.substr(gap + 1), std::atoi(rest.c_str()));
    } else if (name == "proxy-use") {
      proxy->debugUse(std::atoi(rest.c_str()) != 0);
    } else if (name == "proxy-enable") {
      proxy->debugEnable(std::atoi(rest.c_str()));
    } else if (name == "proxy-remove") {
      proxy->debugRemove(std::atoi(rest.c_str()));
    } else {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

}  // namespace Contrail
