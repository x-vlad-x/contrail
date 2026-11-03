#include "background_debug.hpp"

#include "local_state.hpp"
#include "main_window.hpp"
#include "memory_guard.hpp"
#include "settings.hpp"
#include "tray.hpp"

#include <cstdlib>

namespace Contrail {

bool RunBackgroundCommand(MainWindow &window, const std::string &name, const std::string &rest) {
  if (name == "bg-state") {
    const auto *tray = window.tray();
    g_message("bg: window=%d visible=%d background=%d tray=%s unread=%d chat=%lld", int(window.widget() != nullptr),
              window.widget() ? int(gtk_widget_get_visible(window.widget())) : 0, int(Settings().background),
              tray ? (tray->shown() ? "registered" : "waiting") : "none", tray ? tray->unread() : -1,
              static_cast<long long>(window.currentChat()));
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
  } else {
    return false;
  }
  return true;
}

}  // namespace Contrail
